#include "positions.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace asgard {

// ---------------------------------------------------------------------------
// apply_trade
// ---------------------------------------------------------------------------

double apply_trade(UserAccount& user,
                   const Trade& t,
                   bool is_buyer,
                   const Instrument& inst,
                   double mark_price) {
    PositionSide trade_side = is_buyer ? PositionSide::LONG : PositionSide::SHORT;
    double realized_pnl = 0.0;

    auto it = user.positions.find(t.instrument);

    if (it == user.positions.end()) {
        // ----------------------------------------------------------------
        // Case A: Opening a new position
        // ----------------------------------------------------------------
        Position pos;
        pos.user_id      = user.user_id;
        pos.instrument   = t.instrument;
        pos.side         = trade_side;
        pos.size         = t.quantity;
        pos.entry_price  = t.price;
        pos.margin_mode  = MarginMode::CROSS;
        pos.state        = PositionState::OPEN;
        // Initial margin for the new position is already reserved as position_margin;
        // open_order_margin was released by the matching engine.
        user.positions[t.instrument] = pos;
    } else {
        Position& pos = it->second;

        if (pos.side == trade_side) {
            // ----------------------------------------------------------------
            // Case B: Increasing an existing position (same direction)
            // ----------------------------------------------------------------
            double new_entry = (pos.entry_price * pos.size + t.price * t.quantity)
                              / (pos.size + t.quantity);
            pos.size       += t.quantity;
            pos.entry_price = new_entry;
        } else if (t.quantity < pos.size - 1e-9) {
            // ----------------------------------------------------------------
            // Case C: Partial reduction (opposite direction, qty < position)
            // ----------------------------------------------------------------
            double dir = (pos.side == PositionSide::LONG) ? 1.0 : -1.0;
            realized_pnl = dir * (t.price - pos.entry_price) * t.quantity;
            user.wallet_balance += realized_pnl;
            pos.size -= t.quantity;
        } else {
            // ----------------------------------------------------------------
            // Case D: Close + possible flip
            // ----------------------------------------------------------------
            double close_qty = pos.size;
            double dir = (pos.side == PositionSide::LONG) ? 1.0 : -1.0;
            realized_pnl = dir * (t.price - pos.entry_price) * close_qty;
            user.wallet_balance += realized_pnl;

            double flip_qty = t.quantity - close_qty;
            if (flip_qty > 1e-9) {
                // Open new position in opposite direction
                Position new_pos;
                new_pos.user_id     = user.user_id;
                new_pos.instrument  = t.instrument;
                new_pos.side        = trade_side;
                new_pos.size        = flip_qty;
                new_pos.entry_price = t.price;
                new_pos.margin_mode = pos.margin_mode;
                new_pos.state       = PositionState::OPEN;
                user.positions[t.instrument] = new_pos;
            } else {
                // Exact close
                user.positions.erase(it);
            }
        }
    }

    // Deduct trading fee
    bool is_taker = (is_buyer ? t.buyer_is_taker : !t.buyer_is_taker);
    double fee_rate = is_taker ? inst.taker_fee_rate : inst.maker_fee_rate;
    double fee = t.quantity * t.price * fee_rate;
    user.wallet_balance -= fee;

    (void)mark_price; // mark_price reserved for future re-margining logic
    return realized_pnl;
}

// ---------------------------------------------------------------------------
// zero_sum_check
// ---------------------------------------------------------------------------

double zero_sum_check(const std::vector<UserAccount*>& users,
                      double mark_price,
                      double fee_revenue,
                      double vault_balance,
                      double total_deposited) {
    double sum_equity = 0.0;
    for (const auto* u : users) {
        sum_equity += u->wallet_balance;
        for (auto& [sym, pos] : u->positions) {
            double dir = (pos.side == PositionSide::LONG) ? 1.0 : -1.0;
            sum_equity += dir * (mark_price - pos.entry_price) * pos.size;
        }
    }
    return sum_equity + fee_revenue + vault_balance - total_deposited;
}

// ---------------------------------------------------------------------------
// oi_delta
// ---------------------------------------------------------------------------

double oi_delta(const Trade& t,
                const UserAccount& buyer,
                const UserAccount& seller) {
    // Before this trade, what position did each party have?
    auto buyer_pos_it  = buyer.positions.find(t.instrument);
    auto seller_pos_it = seller.positions.find(t.instrument);

    bool buyer_was_long  = buyer_pos_it  != buyer.positions.end()
                        && buyer_pos_it->second.side == PositionSide::LONG;
    bool seller_was_short = seller_pos_it != seller.positions.end()
                         && seller_pos_it->second.side == PositionSide::SHORT;

    // If both are opening new positions → OI increases
    bool buyer_opening  = (buyer_pos_it  == buyer.positions.end()) || !buyer_was_long;
    bool seller_opening = (seller_pos_it == seller.positions.end()) || !seller_was_short;

    if (buyer_opening && seller_opening)  return  t.quantity;
    if (!buyer_opening && !seller_opening) return -t.quantity;
    return 0.0; // one opening, one closing → OI unchanged
}

} // namespace asgard
