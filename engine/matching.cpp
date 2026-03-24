#include "matching.hpp"
#include <algorithm>
#include <cassert>
#include <charconv>
#include <cmath>
#include <cstring>

namespace asgard {

// ---------------------------------------------------------------------------
// make_trade_id  — replaces std::ostringstream with std::to_chars
//
// Produces "T-XXXXXXXX" (zero-padded to 8 digits).
// std::to_chars is locale-free and allocation-free; the 10-char result fits
// within the SSO buffer of std::string so no heap allocation occurs.
// ---------------------------------------------------------------------------

static std::string make_trade_id(uint64_t n) {
    char buf[11];          // "T-" + 8 digits + null
    buf[0] = 'T';
    buf[1] = '-';

    // Write digits right-to-left into a temporary region, then memmove.
    char* const field_beg = buf + 2;
    char* const field_end = buf + 10;
    auto [ptr, ec] = std::to_chars(field_beg, field_end, n);
    (void)ec;

    int digits = static_cast<int>(ptr - field_beg);
    int pad    = 8 - digits;
    if (pad > 0) {
        std::memmove(field_beg + pad, field_beg,
                     static_cast<std::size_t>(digits));
        std::memset(field_beg, '0', static_cast<std::size_t>(pad));
    }

    return std::string(buf, 10);   // SSO: no heap allocation
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Non-const access to accounts_ (held const& to express non-ownership;
// mutated for margin and fill accounting).
static UserAccount& mutable_account(
    const std::unordered_map<std::string, UserAccount>& accounts,
    const std::string& user_id)
{
    return const_cast<std::unordered_map<std::string, UserAccount>&>(accounts)
               .at(user_id);
}

// ---------------------------------------------------------------------------
// violates_impact_band  — Layer 2 check per fill
// ---------------------------------------------------------------------------

bool MatchingEngine::violates_impact_band(double fill_price,
                                           double arrival_best,
                                           OrderSide aggressive_side) const {
    if (arrival_best <= 0.0) return false;
    const double band = inst_.impact_band_pct;
    if (aggressive_side == OrderSide::BUY)
        return fill_price > arrival_best * (1.0 + band) + 1e-9;
    else
        return fill_price < arrival_best * (1.0 - band) - 1e-9;
}

// ---------------------------------------------------------------------------
// handle_stp
//
// SAFETY: for CANCEL_RESTING and CANCEL_BOTH the resting node is erased from
// the LevelList by book_.remove_order().  All fields we need from `resting`
// are therefore saved into locals BEFORE the removal call.
// ---------------------------------------------------------------------------

bool MatchingEngine::handle_stp(Order& incoming, Order& resting,
                                  UserAccount& /*incoming_user*/,
                                  UserAccount& resting_user) {
    switch (incoming.stp_mode) {
        case STPMode::CANCEL_INCOMING:
            incoming.status = OrderStatus::CANCELLED;
            return false;

        case STPMode::CANCEL_RESTING: {
            // Save before list::erase() invalidates the reference.
            const std::string id  = resting.order_id;
            const double      qty = resting.remaining_qty;
            const double      px  = resting.price;
            const OrderSide   sd  = resting.side;

            release_order_margin(resting_user, resting, qty);
            book_.remove_order(id, px, sd);        // may erase the list node
            resting_user.open_orders.erase(id);
            return true;    // continue matching; fetch fresh front next iteration
        }

        case STPMode::CANCEL_BOTH: {
            incoming.status = OrderStatus::CANCELLED;
            const std::string id  = resting.order_id;
            const double      qty = resting.remaining_qty;
            const double      px  = resting.price;
            const OrderSide   sd  = resting.side;

            release_order_margin(resting_user, resting, qty);
            book_.remove_order(id, px, sd);        // may erase the list node
            resting_user.open_orders.erase(id);
            return false;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// make_trade
// ---------------------------------------------------------------------------

Trade MatchingEngine::make_trade(const Order& incoming, const Order& resting,
                                  double fill_qty) {
    Trade t;
    ++trade_seq_;
    t.trade_id     = make_trade_id(trade_seq_);
    t.instrument   = incoming.instrument;
    t.price        = resting.price;    // fills at the passive (resting) price
    t.quantity     = fill_qty;
    t.timestamp_us = now_us();

    if (incoming.side == OrderSide::BUY) {
        t.buyer_id        = incoming.user_id;
        t.buyer_order_id  = incoming.order_id;
        t.seller_id       = resting.user_id;
        t.seller_order_id = resting.order_id;
        t.buyer_is_taker  = true;
    } else {
        t.buyer_id        = resting.user_id;
        t.buyer_order_id  = resting.order_id;
        t.seller_id       = incoming.user_id;
        t.seller_order_id = incoming.order_id;
        t.buyer_is_taker  = false;
    }
    t.seq = seq_.next("TRADE", "{}");
    trade_counter_.fetch_add(1, std::memory_order_relaxed);
    return t;
}

// ---------------------------------------------------------------------------
// release_order_margin / reserve_order_margin
// ---------------------------------------------------------------------------

void MatchingEngine::release_order_margin(UserAccount& user,
                                           const Order& o, double qty) {
    if (qty <= 0.0) return;
    const double ref     = (o.price > 0.0) ? o.price : 0.0;
    const double notional = qty * ref;
    const double im       = calc_im(notional, inst_);
    user.open_order_margin = std::max(0.0, user.open_order_margin - im);
}

void MatchingEngine::reserve_order_margin(UserAccount& user,
                                           const Order& o, double mark_price) {
    const double ref      = (o.price > 0.0) ? o.price : mark_price;
    const double notional = o.remaining_qty * ref;
    const double im       = calc_im(notional, inst_);
    user.open_order_margin += im;
}

// ---------------------------------------------------------------------------
// try_match  — core matching hot path
//
// Optimisations vs. the prior implementation:
//
//  1. best_level_ptr()      One map operation per iteration instead of
//                           best_ask() + peek_front(price) (two lookups).
//
//  2. consume_level_front() On level exhaustion calls erase(begin()) instead
//                           of find(price) + erase.  For partial fills, zero
//                           map operations.
//
//  3. Order& resting        Reference into the list node, not a copy (~200 B).
//                           Fields needed after consumption are saved to
//                           stack locals before consume_level_front().
//
//  4. trades.reserve(8)     Avoids reallocations for typical sweep depth.
// ---------------------------------------------------------------------------

std::vector<Trade> MatchingEngine::try_match(Order& incoming,
                                               double /*mark_price*/) {
    std::vector<Trade> trades;
    trades.reserve(8);

    while (incoming.remaining_qty > 1e-9) {

        // 1. Get best level and its price in a single map operation.
        double      best_price = 0.0;
        PriceLevel* lvl        = book_.best_level_ptr(incoming.side, best_price);
        if (!lvl) break;

        // 2. Price cross check.
        bool crosses;
        if (incoming.type == OrderType::MARKET) {
            crosses = true;
        } else if (incoming.side == OrderSide::BUY) {
            crosses = (incoming.price >= best_price - 1e-9);
        } else {
            crosses = (incoming.price <= best_price + 1e-9);
        }
        if (!crosses) break;

        // 3. Layer 2: impact band.
        if (violates_impact_band(best_price, incoming.arrival_best, incoming.side))
            break;

        // 4. Get a direct reference to the FIFO head order — no copy.
        Order& resting = lvl->orders.front();

        // 5. STP check.  handle_stp saves resting fields before any removal.
        if (resting.user_id == incoming.user_id) {
            auto& inc_user = mutable_account(accounts_, incoming.user_id);
            auto& rst_user = mutable_account(accounts_, resting.user_id);
            bool cont = handle_stp(incoming, resting, inc_user, rst_user);
            if (!cont) break;
            continue;    // resting removed; re-fetch best front next iteration
        }

        // 6. Reduce-only constraint on the incoming order.
        double max_fill_qty = incoming.remaining_qty;
        if (incoming.reduce_only) {
            auto acct_it = accounts_.find(incoming.user_id);
            if (acct_it != accounts_.end()) {
                auto pos_it = acct_it->second.positions.find(incoming.instrument);
                max_fill_qty = (pos_it != acct_it->second.positions.end())
                    ? std::min(max_fill_qty, pos_it->second.size)
                    : 0.0;
            }
        }
        if (max_fill_qty < 1e-9) break;

        const double fill_qty = std::min(max_fill_qty, resting.remaining_qty);

        // 7. Emit trade.  Reads resting fields; safe before consume_level_front.
        trades.push_back(make_trade(incoming, resting, fill_qty));

        // 8. Save resting metadata BEFORE consume_level_front may erase the node.
        auto&             resting_user  = mutable_account(accounts_, resting.user_id);
        const bool        fully_filled  = (fill_qty >= resting.remaining_qty - 1e-9);
        const std::string rst_oid       = resting.order_id;

        release_order_margin(resting_user, resting, fill_qty);

        // 9. Consume from book.  For full fill, erases the list node (resting
        //    reference is dangling after this call for a fully-filled order).
        book_.consume_level_front(incoming.side, lvl, fill_qty);
        incoming.remaining_qty -= fill_qty;

        // 10. Sync resting user's open_orders.
        if (fully_filled) {
            resting_user.open_orders.erase(rst_oid);
        } else {
            auto& oo = resting_user.open_orders[rst_oid];
            oo.remaining_qty -= fill_qty;
            oo.status = OrderStatus::PARTIAL;
        }
    }

    return trades;
}

// ---------------------------------------------------------------------------
// process  — main entry point
// ---------------------------------------------------------------------------

std::vector<Trade> MatchingEngine::process(Order& incoming, double mark_price) {
    // Record the arrival best price for Layer 2 impact band.
    if (incoming.side == OrderSide::BUY) {
        auto ba = book_.best_ask();
        incoming.arrival_best = ba ? *ba : 0.0;
    } else {
        auto bb = book_.best_bid();
        incoming.arrival_best = bb ? *bb : std::numeric_limits<double>::max();
    }

    auto& user = mutable_account(accounts_, incoming.user_id);

    // POST_ONLY: reject if it would cross immediately.
    if (incoming.type == OrderType::POST_ONLY) {
        if (book_.would_cross(incoming)) {
            incoming.status = OrderStatus::REJECTED;
            return {};
        }
        // Falls through to GTC rest logic (no match attempted).
    }

    // FOK pre-check: sufficient depth must exist before we attempt any fills.
    if (incoming.tif == TIF::FOK) {
        double avail = book_.available_qty(incoming.side, incoming.price);
        if (avail < incoming.remaining_qty - 1e-9) {
            incoming.status = OrderStatus::CANCELLED;
            return {};
        }
    }

    // Matching loop.
    std::vector<Trade> trades;
    if (incoming.type != OrderType::POST_ONLY) {
        trades = try_match(incoming, mark_price);
    }

    // Handle residual qty.
    if (incoming.remaining_qty > 1e-9) {
        switch (incoming.tif) {
            case TIF::GTC:
                incoming.status = OrderStatus::OPEN;
                book_.add_order(incoming);
                reserve_order_margin(user, incoming, mark_price);
                user.open_orders[incoming.order_id] = incoming;
                break;
            case TIF::IOC:
            case TIF::FOK:
                incoming.status = trades.empty() ? OrderStatus::CANCELLED
                                                 : OrderStatus::PARTIAL;
                break;
        }
        // POST_ONLY GTC that passed the crossing check rests here.
        if (incoming.type == OrderType::POST_ONLY
                && incoming.tif == TIF::GTC
                && incoming.status != OrderStatus::OPEN) {
            incoming.status = OrderStatus::OPEN;
            book_.add_order(incoming);
            reserve_order_margin(user, incoming, mark_price);
            user.open_orders[incoming.order_id] = incoming;
        }
    } else {
        incoming.status = OrderStatus::FILLED;
    }

    user.messages_this_sec++;
    return trades;
}

// ---------------------------------------------------------------------------
// cancel
// ---------------------------------------------------------------------------

bool MatchingEngine::cancel(const std::string& order_id,
                             UserAccount& user,
                             const std::string& /*instrument*/) {
    auto it = user.open_orders.find(order_id);
    if (it == user.open_orders.end()) return false;

    Order& o = it->second;
    release_order_margin(user, o, o.remaining_qty);
    o.status = OrderStatus::CANCELLED;
    book_.remove_order(order_id, o.price, o.side);
    user.open_orders.erase(it);
    return true;
}

// ---------------------------------------------------------------------------
// cancel_for_modify
// ---------------------------------------------------------------------------

bool MatchingEngine::cancel_for_modify(const std::string& old_order_id,
                                        UserAccount& user) {
    return cancel(old_order_id, user, "");
}

} // namespace asgard
