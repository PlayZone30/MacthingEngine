#pragma once

#include "models.hpp"
#include "margin.hpp"
#include <vector>

namespace asgard {

// ---------------------------------------------------------------------------
// Post-trade processing
//
// apply_trade() must be called ONCE for each side of every trade.
// Returns realized PnL for the given side.
// ---------------------------------------------------------------------------

// Apply a trade fill to a user's account:
//   1. Update position (4 cases: open, increase, reduce, flip)
//   2. Realize PnL and credit wallet_balance
//   3. Deduct trading fee from wallet_balance
//
// is_buyer: true for the buyer side, false for the seller side
// mark_price: current mark (used to compute margin after position change)
double apply_trade(UserAccount& user,
                   const Trade& t,
                   bool is_buyer,
                   const Instrument& inst,
                   double mark_price);

// Verify zero-sum invariant:
//   Σ wallet_balance + Σ unrealized_pnl + fee_revenue + vault_balance ≈ total_deposited
// Returns the discrepancy (should be near zero).
double zero_sum_check(const std::vector<UserAccount*>& users,
                      double mark_price,
                      double fee_revenue,
                      double vault_balance,
                      double total_deposited);

// Compute open interest delta for a given trade:
//   Both opening → +qty
//   One open, one close → 0
//   Both closing → -qty
// Returns the OI delta (+/-).
double oi_delta(const Trade& t,
                const UserAccount& buyer,
                const UserAccount& seller);

} // namespace asgard
