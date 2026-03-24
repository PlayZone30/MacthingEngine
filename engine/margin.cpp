#include "margin.hpp"
#include <algorithm>
#include <stdexcept>

namespace asgard {

// ---------------------------------------------------------------------------
// Core math
// ---------------------------------------------------------------------------

double calc_imf(double notional, const Instrument& inst) {
    return std::max(inst.base_imf, inst.imf_factor * std::sqrt(notional));
}

double calc_mmf(double notional, const Instrument& inst) {
    return std::max(inst.base_mmf, inst.mmf_factor * std::sqrt(notional));
}

double calc_im(double notional, const Instrument& inst) {
    return calc_imf(notional, inst) * notional;
}

double calc_mm(double notional, const Instrument& inst) {
    return calc_mmf(notional, inst) * notional;
}

// ---------------------------------------------------------------------------
// Unrealized PnL
// ---------------------------------------------------------------------------

double unrealized_pnl(const Position& pos, double mark_price) {
    double dir = (pos.side == PositionSide::LONG) ? 1.0 : -1.0;
    return dir * (mark_price - pos.entry_price) * pos.size;
}

// ---------------------------------------------------------------------------
// Equity
// ---------------------------------------------------------------------------

double cross_equity(const UserAccount& user, double mark_price) {
    double upnl = 0.0;
    for (auto& [sym, pos] : user.positions) {
        if (pos.margin_mode == MarginMode::CROSS)
            upnl += unrealized_pnl(pos, mark_price);
    }
    return user.wallet_balance + upnl;
}

// ---------------------------------------------------------------------------
// Position margin
// ---------------------------------------------------------------------------

double position_mm(const Position& pos, double mark_price, const Instrument& inst) {
    double notional = pos.size * mark_price;
    return calc_mm(notional, inst);
}

double position_im(const Position& pos, double mark_price, const Instrument& inst) {
    double notional = pos.size * mark_price;
    return calc_im(notional, inst);
}

// ---------------------------------------------------------------------------
// Totals (cross-margin only)
// ---------------------------------------------------------------------------

double total_cross_mm(const UserAccount& user, double mark_price, const Instrument& inst) {
    double total = 0.0;
    for (auto& [sym, pos] : user.positions) {
        if (pos.margin_mode == MarginMode::CROSS)
            total += position_mm(pos, mark_price, inst);
    }
    return total;
}

double total_cross_im(const UserAccount& user, double mark_price, const Instrument& inst) {
    double total = 0.0;
    for (auto& [sym, pos] : user.positions) {
        if (pos.margin_mode == MarginMode::CROSS)
            total += position_im(pos, mark_price, inst);
    }
    return total;
}

// ---------------------------------------------------------------------------
// Available balance
// ---------------------------------------------------------------------------

double available_balance(const UserAccount& user, double mark_price, const Instrument& inst) {
    double eq = cross_equity(user, mark_price);
    double pm = total_cross_im(user, mark_price, inst);
    return eq - pm - user.open_order_margin;
}

// ---------------------------------------------------------------------------
// Additional IM required for a new order
// ---------------------------------------------------------------------------

double additional_im_for_order(const Order& o,
                                const UserAccount& user,
                                const Instrument& inst,
                                double mark_price) {
    // Find existing position (if any) in this instrument
    auto it = user.positions.find(o.instrument);

    // Reference price: use order price for limit, mark for market
    double ref_price = (o.type == OrderType::MARKET || o.price == 0.0)
                       ? mark_price : o.price;
    double order_notional = o.quantity * ref_price;
    double order_im = calc_im(order_notional, inst);

    if (it == user.positions.end()) {
        // No existing position → opening new
        return order_im;
    }

    const Position& pos = it->second;

    bool same_dir = (o.side == OrderSide::BUY  && pos.side == PositionSide::LONG) ||
                    (o.side == OrderSide::SELL && pos.side == PositionSide::SHORT);

    if (same_dir) {
        // Increasing position → need IM for the additional size
        return order_im;
    }

    // Reducing or flipping
    if (o.quantity <= pos.size) {
        // Pure reduce → frees margin, no additional IM needed
        return 0.0;
    }

    // Flip: close existing, open new in opposite direction
    double flip_qty     = o.quantity - pos.size;
    double flip_notional = flip_qty * ref_price;
    double flip_im       = calc_im(flip_notional, inst);

    // IM freed from closing existing position
    double close_notional = pos.size * mark_price;
    double freed_im        = calc_im(close_notional, inst);

    double net = flip_im - freed_im;
    return std::max(0.0, net);
}

// ---------------------------------------------------------------------------
// Liquidation price estimate
// ---------------------------------------------------------------------------

double liq_price_estimate(const Position& pos,
                           double effective_equity,
                           const Instrument& inst) {
    if (pos.size < 1e-9) return 0.0;

    // Approximate: find price P where equity = MM(P)
    // Simplified linear approximation (ignores sqrt nonlinearity):
    // equity_at_P = equity_now + dir*(P - mark)*size  (not quite right, but close)
    // For display purposes this is standard practice.

    // Using: liq_price ≈ entry ± (effective_equity - MM) / size
    // where MM is computed at current mark (approximate, iterative would be exact)
    double base_mm = inst.base_mmf; // simplified: use base MMF
    double dir = (pos.side == PositionSide::LONG) ? -1.0 : 1.0;

    // Effective equity must cover just the MM at liq price:
    // effective_equity - size*(liq_price - entry_price)*(-dir) = MM_at_liq
    // linear approx: liq_price ≈ entry + dir*(effective_equity - base_mm * entry_price * size) / size
    double liq = pos.entry_price + dir * (effective_equity / pos.size - base_mm * pos.entry_price);
    return std::max(0.0, liq);
}

// ---------------------------------------------------------------------------
// Bankruptcy price
// ---------------------------------------------------------------------------

double bankruptcy_price(const Position& pos, double allocated_margin) {
    if (pos.size < 1e-9) return 0.0;
    if (pos.side == PositionSide::LONG)
        return pos.entry_price - (allocated_margin / pos.size);
    else
        return pos.entry_price + (allocated_margin / pos.size);
}

} // namespace asgard
