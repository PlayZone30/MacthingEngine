#include "liquidation.hpp"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>

namespace asgard {

// ---------------------------------------------------------------------------
// cancel_all_orders
// ---------------------------------------------------------------------------

void LiquidationEngine::cancel_all_orders(UserAccount& user,
                                           const std::string& instrument,
                                           MarginMode mode) {
    std::vector<std::string> to_cancel;
    for (auto& [id, o] : user.open_orders) {
        if (mode == MarginMode::CROSS || o.instrument == instrument) {
            to_cancel.push_back(id);
        }
    }
    for (auto& id : to_cancel) {
        engine_.cancel(id, user, instrument);
    }
}

// ---------------------------------------------------------------------------
// execute_chunk  — one liquidation IOC order
// ---------------------------------------------------------------------------

std::vector<Trade> LiquidationEngine::execute_chunk(UserAccount& user,
                                                      Position& pos,
                                                      double chunk_qty,
                                                      double mark_price) {
    // Price band for liquidation order
    double liq_limit;
    OrderSide liq_side;
    if (pos.side == PositionSide::LONG) {
        // Selling the long: limit = mark * (1 - liq_band%)
        liq_side  = OrderSide::SELL;
        liq_limit = mark_price * (1.0 - inst_.liq_band_pct);
    } else {
        // Buying back the short: limit = mark * (1 + liq_band%)
        liq_side  = OrderSide::BUY;
        liq_limit = mark_price * (1.0 + inst_.liq_band_pct);
    }

    static uint64_t liq_order_counter = 0;
    std::ostringstream oss;
    oss << "LIQ-" << std::setfill('0') << std::setw(8) << (++liq_order_counter);

    Order liq_order;
    liq_order.order_id     = oss.str();
    liq_order.user_id      = user.user_id;
    liq_order.instrument   = pos.instrument;
    liq_order.side         = liq_side;
    liq_order.type         = OrderType::LIMIT;
    liq_order.price        = liq_limit;
    liq_order.quantity     = chunk_qty;
    liq_order.remaining_qty = chunk_qty;
    liq_order.tif          = TIF::IOC;
    liq_order.reduce_only  = true;
    liq_order.status       = OrderStatus::NEW;
    liq_order.timestamp_us = now_us();

    return engine_.process(liq_order, mark_price);
}

// ---------------------------------------------------------------------------
// trigger
// ---------------------------------------------------------------------------

int LiquidationEngine::trigger(const std::string& user_id,
                                 const std::string& instrument,
                                 double mark_price) {
    auto acc_it = accounts_.find(user_id);
    if (acc_it == accounts_.end()) return 0;
    UserAccount& user = acc_it->second;

    auto pos_it = user.positions.find(instrument);
    if (pos_it == user.positions.end()) return 0;
    Position& pos = pos_it->second;

    if (pos.state == PositionState::LIQUIDATING) return 0; // already in progress
    pos.state = PositionState::LIQUIDATING;

    // ------------------------------------------------------------------
    // Step 1: Cancel all open orders
    // ------------------------------------------------------------------
    MarginMode mode = pos.margin_mode;
    cancel_all_orders(user, instrument, mode);

    // ------------------------------------------------------------------
    // Step 2: Re-check equity after cancellations
    // ------------------------------------------------------------------
    double eq    = cross_equity(user, mark_price);
    double total_mm = total_cross_mm(user, mark_price, inst_);

    if (eq > total_mm + 1e-9) {
        // Margin restored by cancelling orders — no liquidation needed
        pos.state = PositionState::OPEN;
        return 0;
    }

    // ------------------------------------------------------------------
    // Steps 3-7: Gradual liquidation loop (~10% per chunk)
    // ------------------------------------------------------------------
    int chunks_executed = 0;
    const int MAX_ITERATIONS = 100; // safety cap

    while (pos.size > 1e-9 && chunks_executed < MAX_ITERATIONS) {
        double chunk = std::max(inst_.min_lot, pos.size * inst_.liq_chunk_pct);
        chunk = std::min(chunk, pos.size); // cap at remaining size

        auto trades = execute_chunk(user, pos, chunk, mark_price);
        ++liq_count_;
        ++chunks_executed;

        for (auto& t : trades) {
            // Deduct liquidation fee
            double liq_fee = t.quantity * t.price * inst_.liq_fee_rate;
            user.wallet_balance -= liq_fee;
            vault_.credit_fee(liq_fee);

            // Check for bankruptcy
            double alloc_margin = (pos.margin_mode == MarginMode::ISOLATED)
                ? pos.allocated_margin
                : position_im(pos, mark_price, inst_);

            double bk_price = bankruptcy_price(pos, alloc_margin);
            bool went_bankrupt = (pos.side == PositionSide::LONG)
                ? t.price < bk_price - 1e-9
                : t.price > bk_price + 1e-9;

            if (went_bankrupt) {
                double deficit = std::abs(t.price - bk_price) * t.quantity;
                bool vault_ok = vault_.absorb_deficit(deficit);
                if (!vault_ok) {
                    // Layer 3: ADL
                    run_adl(pos.side, instrument, deficit, mark_price);
                }
            }

            // Update position via apply_trade
            bool is_buyer = (t.buyer_id == user_id);
            apply_trade(user, t, is_buyer, inst_, mark_price);
        }

        // Re-check if margin is restored
        if (user.positions.find(instrument) == user.positions.end()) break; // fully closed
        pos_it = user.positions.find(instrument);
        if (pos_it == user.positions.end()) break;
        pos = pos_it->second; // refresh reference after map mutation

        eq       = cross_equity(user, mark_price);
        total_mm = total_cross_mm(user, mark_price, inst_);
        if (eq > total_mm + 1e-9) break; // margin restored
    }

    // ------------------------------------------------------------------
    // Step 5: Settlement (user equity < 0 → vault absorbs remaining)
    // ------------------------------------------------------------------
    if (user.wallet_balance < 0.0) {
        double deficit = -user.wallet_balance;
        vault_.absorb_deficit(deficit);
        user.wallet_balance = 0.0;
    }

    // Update final position state
    pos_it = user.positions.find(instrument);
    if (pos_it != user.positions.end()) {
        pos_it->second.state = (pos_it->second.size < 1e-9)
                               ? PositionState::CLOSED
                               : PositionState::OPEN;
    }

    return chunks_executed;
}

// ---------------------------------------------------------------------------
// rank_adl_candidates
// ---------------------------------------------------------------------------

std::vector<ADLCandidate>
LiquidationEngine::rank_adl_candidates(PositionSide bankrupt_side,
                                        const std::string& instrument,
                                        double mark_price) const {
    // Opposite side of bankrupt position profits the most
    PositionSide target_side = (bankrupt_side == PositionSide::LONG)
                               ? PositionSide::SHORT : PositionSide::LONG;

    std::vector<ADLCandidate> candidates;
    for (auto& [uid, user] : accounts_) {
        auto it = user.positions.find(instrument);
        if (it == user.positions.end()) continue;
        const Position& pos = it->second;
        if (pos.side != target_side) continue;
        if (pos.state == PositionState::LIQUIDATING) continue;

        double upnl    = unrealized_pnl(pos, mark_price);
        if (upnl <= 0.0) continue; // only profitable traders are ADL candidates

        double pos_imv = position_im(pos, mark_price, inst_);
        double eq      = cross_equity(user, mark_price);

        ADLCandidate c;
        c.user_id     = uid;
        c.instrument  = instrument;
        c.pnl_pct     = (pos_imv > 1e-9) ? upnl / pos_imv : 0.0;
        c.eff_leverage = (eq > 1e-9) ? (pos.size * mark_price) / eq : 0.0;
        c.adl_score   = c.pnl_pct * c.eff_leverage;
        candidates.push_back(c);
    }

    // Sort descending by ADL score (highest priority = deleveraged first)
    std::sort(candidates.begin(), candidates.end(),
        [](const ADLCandidate& a, const ADLCandidate& b){ return a.adl_score > b.adl_score; });
    return candidates;
}

// ---------------------------------------------------------------------------
// run_adl
// ---------------------------------------------------------------------------

void LiquidationEngine::run_adl(PositionSide bankrupt_side,
                                  const std::string& instrument,
                                  double deficit,
                                  double mark_price) {
    auto candidates = rank_adl_candidates(bankrupt_side, instrument, mark_price);
    double remaining = deficit;

    for (auto& c : candidates) {
        if (remaining <= 1e-9) break;

        auto& target = accounts_.at(c.user_id);
        auto pos_it  = target.positions.find(instrument);
        if (pos_it == target.positions.end()) continue;

        Position& tpos = pos_it->second;
        double adl_qty = std::min(tpos.size, remaining / mark_price);
        adl_qty = std::min(adl_qty, tpos.size);

        // Force-close at mark price
        double pnl_dir = (tpos.side == PositionSide::LONG) ? 1.0 : -1.0;
        double realized = pnl_dir * (mark_price - tpos.entry_price) * adl_qty;
        target.wallet_balance += realized;
        tpos.size -= adl_qty;
        if (tpos.size < 1e-9) target.positions.erase(pos_it);

        remaining -= adl_qty * mark_price;
        if (on_adl_) on_adl_(c.user_id, instrument, adl_qty, mark_price);
    }
}

} // namespace asgard
