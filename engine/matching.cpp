#include "matching.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <sstream>
#include <iomanip>

namespace asgard {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string make_trade_id(uint64_t n) {
    std::ostringstream oss;
    oss << "T-" << std::setfill('0') << std::setw(8) << n;
    return oss.str();
}

// Non-const access to accounts_ is needed for margin adjustments.
// We hold a const& in the header for the general case; for mutations we
// cast away const (the accounts map is logically owned by the engine).
static UserAccount& mutable_account(
    const std::unordered_map<std::string, UserAccount>& accounts,
    const std::string& user_id)
{
    return const_cast<std::unordered_map<std::string, UserAccount>&>(accounts).at(user_id);
}

// ---------------------------------------------------------------------------
// violates_impact_band  — Layer 2 check per fill
// ---------------------------------------------------------------------------

bool MatchingEngine::violates_impact_band(double fill_price,
                                           double arrival_best,
                                           OrderSide aggressive_side) const {
    if (arrival_best <= 0.0) return false;
    double band = inst_.impact_band_pct;
    if (aggressive_side == OrderSide::BUY)
        return fill_price > arrival_best * (1.0 + band) + 1e-9;
    else
        return fill_price < arrival_best * (1.0 - band) - 1e-9;
}

// ---------------------------------------------------------------------------
// handle_stp
// ---------------------------------------------------------------------------

bool MatchingEngine::handle_stp(Order& incoming, Order& resting,
                                  UserAccount& /*incoming_user*/,
                                  UserAccount& resting_user) {
    switch (incoming.stp_mode) {
        case STPMode::CANCEL_INCOMING:
            incoming.status = OrderStatus::CANCELLED;
            return false;  // stop matching

        case STPMode::CANCEL_RESTING: {
            // Remove resting order from the book, release its margin
            release_order_margin(resting_user, resting, resting.remaining_qty);
            resting.status = OrderStatus::CANCELLED;
            book_.remove_order(resting.order_id, resting.price, resting.side);
            resting_user.open_orders.erase(resting.order_id);
            return true;   // continue matching (resting removed)
        }

        case STPMode::CANCEL_BOTH:
            incoming.status = OrderStatus::CANCELLED;
            release_order_margin(resting_user, resting, resting.remaining_qty);
            resting.status = OrderStatus::CANCELLED;
            book_.remove_order(resting.order_id, resting.price, resting.side);
            resting_user.open_orders.erase(resting.order_id);
            return false;  // stop matching
    }
    return false;
}

// ---------------------------------------------------------------------------
// make_trade
// ---------------------------------------------------------------------------

Trade MatchingEngine::make_trade(const Order& incoming, const Order& resting, double fill_qty) {
    Trade t;
    ++trade_seq_;
    t.trade_id     = make_trade_id(trade_seq_);
    t.instrument   = incoming.instrument;
    t.price        = resting.price;  // fills always at the resting (passive) price
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
// release_order_margin
// ---------------------------------------------------------------------------

void MatchingEngine::release_order_margin(UserAccount& user,
                                           const Order& o, double qty) {
    if (qty <= 0.0) return;
    double ref  = (o.price > 0.0) ? o.price : 0.0;
    double notional = qty * ref;
    double im   = calc_im(notional, inst_);
    user.open_order_margin = std::max(0.0, user.open_order_margin - im);
}

// ---------------------------------------------------------------------------
// reserve_order_margin
// ---------------------------------------------------------------------------

void MatchingEngine::reserve_order_margin(UserAccount& user,
                                           const Order& o, double mark_price) {
    double ref = (o.price > 0.0) ? o.price : mark_price;
    double notional = o.remaining_qty * ref;
    double im = calc_im(notional, inst_);
    user.open_order_margin += im;
}

// ---------------------------------------------------------------------------
// try_match  — walk opposite book and fill
// ---------------------------------------------------------------------------

std::vector<Trade> MatchingEngine::try_match(Order& incoming, double mark_price) {
    std::vector<Trade> trades;

    while (incoming.remaining_qty > 1e-9) {
        // Get best price on the opposite side
        std::optional<double> best_opt;
        if (incoming.side == OrderSide::BUY)  best_opt = book_.best_ask();
        else                                    best_opt = book_.best_bid();

        if (!best_opt) break; // empty opposite book
        double best_price = *best_opt;

        // Does the incoming order cross?
        bool crosses;
        if (incoming.type == OrderType::MARKET) {
            crosses = true;
        } else if (incoming.side == OrderSide::BUY) {
            crosses = (incoming.price >= best_price - 1e-9);
        } else {
            crosses = (incoming.price <= best_price + 1e-9);
        }
        if (!crosses) break;

        // Layer 2: impact band check
        if (violates_impact_band(best_price, incoming.arrival_best, incoming.side))
            break;

        // Get front of the level (FIFO head)
        Order* resting_ptr = book_.peek_front(incoming.side, best_price);
        if (!resting_ptr) break;
        Order resting_copy = *resting_ptr; // copy for STP check

        // STP check
        if (resting_copy.user_id == incoming.user_id) {
            auto& inc_user = mutable_account(accounts_, incoming.user_id);
            auto& rst_user = mutable_account(accounts_, resting_copy.user_id);
            bool cont = handle_stp(incoming, resting_copy, inc_user, rst_user);
            if (!cont) break;
            // If CANCEL_RESTING, the resting was removed; loop to find the next order
            continue;
        }

        // Reduce-only constraint on incoming
        double max_fill_qty = incoming.remaining_qty;
        if (incoming.reduce_only) {
            auto it = accounts_.find(incoming.user_id);
            if (it != accounts_.end()) {
                auto pos_it = it->second.positions.find(incoming.instrument);
                if (pos_it != it->second.positions.end()) {
                    max_fill_qty = std::min(max_fill_qty, pos_it->second.size);
                } else {
                    max_fill_qty = 0.0; // no position to reduce
                }
            }
        }
        if (max_fill_qty < 1e-9) break;

        double fill_qty = std::min(max_fill_qty, resting_copy.remaining_qty);

        // Create trade
        Trade t = make_trade(incoming, resting_copy, fill_qty);
        trades.push_back(t);

        // Update quantities
        incoming.remaining_qty -= fill_qty;

        // Consume from book (FIFO: remove from front of level)
        book_.consume_front(incoming.side, best_price, fill_qty);

        // Release open_order_margin from the resting order (for the filled portion)
        auto& resting_user = mutable_account(accounts_, resting_copy.user_id);
        release_order_margin(resting_user, resting_copy, fill_qty);
        if (fill_qty >= resting_copy.remaining_qty - 1e-9) {
            // Fully filled
            resting_user.open_orders.erase(resting_copy.order_id);
        } else {
            // Partially filled — update in open_orders map
            auto& oo = resting_user.open_orders[resting_copy.order_id];
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
    // Record arrival best price (for Layer 2 impact band)
    if (incoming.side == OrderSide::BUY) {
        auto ba = book_.best_ask();
        incoming.arrival_best = ba ? *ba : 0.0;
    } else {
        auto bb = book_.best_bid();
        incoming.arrival_best = bb ? *bb : std::numeric_limits<double>::max();
    }

    auto& user = mutable_account(accounts_, incoming.user_id);

    // ---------- POST_ONLY check ----------------------------------------
    if (incoming.type == OrderType::POST_ONLY) {
        if (book_.would_cross(incoming)) {
            incoming.status = OrderStatus::REJECTED;
            return {};
        }
        // Falls through to GTC rest logic below (no matching attempt)
    }

    // ---------- FOK pre-check ------------------------------------------
    if (incoming.tif == TIF::FOK) {
        double avail = book_.available_qty(incoming.side, incoming.price);
        if (avail < incoming.remaining_qty - 1e-9) {
            incoming.status = OrderStatus::CANCELLED;
            return {};
        }
    }

    // ---------- Matching loop ------------------------------------------
    std::vector<Trade> trades;
    if (incoming.type != OrderType::POST_ONLY) {
        trades = try_match(incoming, mark_price);
    }

    // ---------- Handle residual qty ------------------------------------
    if (incoming.remaining_qty > 1e-9) {
        switch (incoming.tif) {
            case TIF::GTC:
                // Rest in book
                incoming.status = OrderStatus::OPEN;
                book_.add_order(incoming);
                reserve_order_margin(user, incoming, mark_price);
                user.open_orders[incoming.order_id] = incoming;
                break;
            case TIF::IOC:
            case TIF::FOK:
                // Cancel remainder
                incoming.status = trades.empty() ? OrderStatus::CANCELLED : OrderStatus::PARTIAL;
                break;
        }
        // POST_ONLY with GTC tif rests in book (already handled above; no match attempted)
        if (incoming.type == OrderType::POST_ONLY && incoming.tif == TIF::GTC
                && incoming.status != OrderStatus::OPEN) {
            incoming.status = OrderStatus::OPEN;
            book_.add_order(incoming);
            reserve_order_margin(user, incoming, mark_price);
            user.open_orders[incoming.order_id] = incoming;
        }
    } else {
        incoming.status = OrderStatus::FILLED;
    }

    // Increment rate limit counter
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
