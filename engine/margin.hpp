#pragma once

#include "models.hpp"
#include <cmath>
#include <string>

namespace asgard {

// ---------------------------------------------------------------------------
// Margin math — sqrt-based IMF/MMF
//
//   IMF = max(base_imf, imf_factor × √notional)
//   IM  = IMF × notional
//
//   MMF = max(base_mmf, mmf_factor × √notional)
//   MM  = MMF × notional
// ---------------------------------------------------------------------------

double calc_imf(double notional, const Instrument& inst);
double calc_mmf(double notional, const Instrument& inst);
double calc_im (double notional, const Instrument& inst);
double calc_mm (double notional, const Instrument& inst);

// Unrealized PnL of a single position given current mark price
double unrealized_pnl(const Position& pos, double mark_price);

// Equity of a user = wallet_balance + Σ unrealized_pnl (cross positions)
// For isolated: effective_equity = allocated_margin + unrealized_pnl of that position
double cross_equity(const UserAccount& user, double mark_price);

// Maintenance margin for a single position at mark_price
double position_mm(const Position& pos, double mark_price, const Instrument& inst);

// Initial margin for a single position at mark_price
double position_im(const Position& pos, double mark_price, const Instrument& inst);

// Total maintenance margin across all cross-margin positions
double total_cross_mm(const UserAccount& user, double mark_price, const Instrument& inst);

// Total initial margin across all cross-margin positions
double total_cross_im(const UserAccount& user, double mark_price, const Instrument& inst);

// Available balance = equity − position_margin − open_order_margin
double available_balance(const UserAccount& user, double mark_price, const Instrument& inst);

// Additional initial margin required for a new order.
// Returns 0 if the order reduces an existing position.
double additional_im_for_order(const Order& o,
                                const UserAccount& user,
                                const Instrument& inst,
                                double mark_price);

// Estimated liquidation price for a position (approximate, linear)
// For cross: cross_equity is equity − MM_of_other_positions
double liq_price_estimate(const Position& pos,
                           double effective_equity,
                           const Instrument& inst);

// Bankruptcy price = price at which equity = 0
// Long:  entry − margin/size
// Short: entry + margin/size
double bankruptcy_price(const Position& pos, double allocated_margin);

} // namespace asgard
