#pragma once

#include "models.hpp"
#include "margin.hpp"
#include <string>

namespace asgard {

// ---------------------------------------------------------------------------
// RiskResult
// ---------------------------------------------------------------------------

struct RiskResult {
    bool        pass = true;
    std::string reason;
};

// ---------------------------------------------------------------------------
// Pre-Trade Risk Engine — 7 ordered checks
//
// All checks run on the engine thread before the order touches the book.
// Check 7 (STP) is evaluated inside the matching engine at match time.
// ---------------------------------------------------------------------------

class RiskEngine {
public:
    // Run all pre-trade checks.  Returns on first failure (fail-fast).
    RiskResult check_all(const Order& o,
                         const UserAccount& user,
                         const Instrument& inst,
                         double mark_price) const;

    // Individual checks (exposed for testing)
    RiskResult check_instrument_state (const Instrument& inst) const;
    RiskResult check_price_band       (const Order& o, const Instrument& inst, double mark) const;
    RiskResult check_size             (const Order& o, const Instrument& inst) const;
    RiskResult check_rate_limit       (const UserAccount& user) const;
    RiskResult check_position_limit   (const Order& o, const UserAccount& user,
                                       const Instrument& inst) const;
    RiskResult check_margin           (const Order& o, const UserAccount& user,
                                       const Instrument& inst, double mark) const;
    RiskResult check_reduce_only      (const Order& o, const UserAccount& user) const;
    RiskResult check_cooldown_mode    (const Order& o, const Instrument& inst) const;
};

} // namespace asgard
