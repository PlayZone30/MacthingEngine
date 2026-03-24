#pragma once

#include "models.hpp"
#include "matching.hpp"
#include "positions.hpp"
#include "margin.hpp"
#include <functional>
#include <string>
#include <vector>

namespace asgard {

// ---------------------------------------------------------------------------
// LiquidationEngine
//
// Implements the 3-layer waterfall:
//   Layer 1: Orderbook (gradual IOC liquidation orders)
//   Layer 2: Insurance vault (absorbs deficits when below bankruptcy price)
//   Layer 3: ADL (last resort when vault is depleted)
// ---------------------------------------------------------------------------

struct ADLCandidate {
    std::string user_id;
    std::string instrument;
    double      pnl_pct       = 0.0;  // unrealized_pnl / position_margin
    double      eff_leverage  = 0.0;  // notional / equity
    double      adl_score     = 0.0;  // pnl_pct × eff_leverage
};

class LiquidationEngine {
public:
    using ADLCallback = std::function<void(const std::string& user_id,
                                            const std::string& instrument,
                                            double fill_qty, double fill_price)>;

    LiquidationEngine(MatchingEngine& engine,
                      InsuranceVault& vault,
                      std::unordered_map<std::string, UserAccount>& accounts,
                      const Instrument& inst)
        : engine_(engine), vault_(vault), accounts_(accounts), inst_(inst) {}

    void set_adl_callback(ADLCallback cb) { on_adl_ = std::move(cb); }

    // Trigger liquidation for a specific user/instrument.
    // Returns number of liquidation chunks executed.
    int trigger(const std::string& user_id,
                const std::string& instrument,
                double mark_price);

    // Compute ADL ranking for all users with positions on the opposite side.
    // bankrupt_side: the side of the bankrupt position (LONG or SHORT)
    std::vector<ADLCandidate> rank_adl_candidates(PositionSide bankrupt_side,
                                                   const std::string& instrument,
                                                   double mark_price) const;

    uint64_t total_liquidations() const { return liq_count_; }

private:
    // Cancel all open orders (cross: all instruments; isolated: just this one).
    void cancel_all_orders(UserAccount& user, const std::string& instrument,
                           MarginMode mode);

    // Execute one liquidation chunk.  Returns the fills produced.
    std::vector<Trade> execute_chunk(UserAccount& user,
                                      Position& pos,
                                      double chunk_qty,
                                      double mark_price);

    // Run ADL for a given deficit.
    void run_adl(PositionSide bankrupt_side,
                 const std::string& instrument,
                 double deficit,
                 double mark_price);

    MatchingEngine& engine_;
    InsuranceVault& vault_;
    std::unordered_map<std::string, UserAccount>& accounts_;
    const Instrument& inst_;
    ADLCallback on_adl_;
    uint64_t liq_count_ = 0;
};

} // namespace asgard
