#include "risk_checks.hpp"
#include <cmath>
#include <algorithm>

namespace asgard {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static double net_position(const UserAccount& user, const std::string& instrument) {
    auto it = user.positions.find(instrument);
    if (it == user.positions.end()) return 0.0;
    const Position& p = it->second;
    return (p.side == PositionSide::LONG) ? p.size : -p.size;
}

// ---------------------------------------------------------------------------
// Check 1: Instrument state
// ---------------------------------------------------------------------------

RiskResult RiskEngine::check_instrument_state(const Instrument& inst) const {
    if (inst.state == InstrumentState::TRADING)
        return {true, ""};
    if (inst.state == InstrumentState::COOLDOWN)
        return {true, ""};   // Cooldown-specific restrictions handled by check_cooldown_mode
    return {false, "Instrument not in TRADING state: " + inst.symbol};
}

// ---------------------------------------------------------------------------
// Check 2: Price band (fat finger protection)
// Market orders skip this check.
// ---------------------------------------------------------------------------

RiskResult RiskEngine::check_price_band(const Order& o,
                                         const Instrument& inst,
                                         double mark) const {
    if (o.type == OrderType::MARKET) return {true, ""}; // market orders bypass

    double band = inst.price_band_pct;
    if (o.side == OrderSide::BUY) {
        double ceiling = mark * (1.0 + band);
        if (o.price > ceiling + 1e-9)
            return {false, "Buy price " + std::to_string(o.price)
                         + " exceeds limit band ceiling " + std::to_string(ceiling)};
    } else {
        double floor = mark * (1.0 - band);
        if (o.price < floor - 1e-9)
            return {false, "Sell price " + std::to_string(o.price)
                         + " below limit band floor " + std::to_string(floor)};
    }
    return {true, ""};
}

// ---------------------------------------------------------------------------
// Check 3: Size validation
// ---------------------------------------------------------------------------

RiskResult RiskEngine::check_size(const Order& o, const Instrument& inst) const {
    if (o.quantity < inst.min_lot - 1e-9)
        return {false, "Quantity below minimum lot size"};
    if (o.quantity > inst.max_lot + 1e-9)
        return {false, "Quantity exceeds maximum order size"};
    // Check lot_step alignment (e.g., must be multiple of 0.001)
    if (inst.lot_step > 1e-12) {
        double rem = std::fmod(o.quantity, inst.lot_step);
        if (rem > 1e-9 && (inst.lot_step - rem) > 1e-9)
            return {false, "Quantity not a multiple of lot step"};
    }
    return {true, ""};
}

// ---------------------------------------------------------------------------
// Check 4: Rate limiting
// ---------------------------------------------------------------------------

RiskResult RiskEngine::check_rate_limit(const UserAccount& user) const {
    if (user.messages_this_sec >= user.rate_limit)
        return {false, "Rate limit exceeded"};
    return {true, ""};
}

// ---------------------------------------------------------------------------
// Check 5: Position limit
// ---------------------------------------------------------------------------

RiskResult RiskEngine::check_position_limit(const Order& o,
                                              const UserAccount& user,
                                              const Instrument& inst) const {
    double current  = net_position(user, o.instrument);
    double sign     = (o.side == OrderSide::BUY) ? 1.0 : -1.0;
    double hyp_pos  = current + sign * o.quantity;

    if (std::abs(hyp_pos) > inst.max_position_size + 1e-9)
        return {false, "Would exceed position limit of "
                     + std::to_string(inst.max_position_size)};
    return {true, ""};
}

// ---------------------------------------------------------------------------
// Check 6: Margin check
// ---------------------------------------------------------------------------

RiskResult RiskEngine::check_margin(const Order& o,
                                     const UserAccount& user,
                                     const Instrument& inst,
                                     double mark) const {
    double add_im = additional_im_for_order(o, user, inst, mark);
    if (add_im <= 0.0) return {true, ""}; // reduce → no margin needed

    double avail = available_balance(user, mark, inst);
    if (avail < add_im - 1e-9)
        return {false, "Insufficient margin: need " + std::to_string(add_im)
                     + " available " + std::to_string(avail)};
    return {true, ""};
}

// ---------------------------------------------------------------------------
// Check 7 (inline): reduce-only validation
// Ensures the order won't open a new position or increase an existing one.
// ---------------------------------------------------------------------------

RiskResult RiskEngine::check_reduce_only(const Order& o, const UserAccount& user) const {
    if (!o.reduce_only) return {true, ""};

    auto it = user.positions.find(o.instrument);
    if (it == user.positions.end())
        return {false, "Reduce-only rejected: no existing position"};

    const Position& pos = it->second;
    // Reduce-only buy must reduce a short; reduce-only sell must reduce a long
    bool is_reducing = (o.side == OrderSide::BUY  && pos.side == PositionSide::SHORT) ||
                       (o.side == OrderSide::SELL && pos.side == PositionSide::LONG);

    if (!is_reducing)
        return {false, "Reduce-only would open or increase position"};

    return {true, ""};
}

// ---------------------------------------------------------------------------
// Check for cooldown-mode restrictions
// During COOLDOWN: market orders and immediately-crossing orders are blocked.
// (Post-Only behavior applied system-wide.)
// ---------------------------------------------------------------------------

RiskResult RiskEngine::check_cooldown_mode(const Order& o, const Instrument& inst) const {
    if (inst.state != InstrumentState::COOLDOWN) return {true, ""};

    if (o.type == OrderType::MARKET)
        return {false, "Market orders rejected during COOLDOWN"};

    // Aggressive limit orders that would immediately match are blocked.
    // (The matching engine will enforce impact bands regardless; we additionally
    //  reject IOC/FOK takers during cooldown to limit aggression.)
    if (o.tif == TIF::IOC || o.tif == TIF::FOK)
        return {false, "IOC/FOK orders rejected during COOLDOWN"};

    return {true, ""};
}

// ---------------------------------------------------------------------------
// check_all — run all checks in order
// ---------------------------------------------------------------------------

RiskResult RiskEngine::check_all(const Order& o,
                                  const UserAccount& user,
                                  const Instrument& inst,
                                  double mark_price) const {
    RiskResult r;

    // 1. Instrument state
    r = check_instrument_state(inst);
    if (!r.pass) return r;

    // 1b. Cooldown restrictions (subset of instrument-state check)
    r = check_cooldown_mode(o, inst);
    if (!r.pass) return r;

    // 2. Price band
    r = check_price_band(o, inst, mark_price);
    if (!r.pass) return r;

    // 3. Size
    r = check_size(o, inst);
    if (!r.pass) return r;

    // 4. Rate limit
    r = check_rate_limit(user);
    if (!r.pass) return r;

    // 5. Position limit
    r = check_position_limit(o, user, inst);
    if (!r.pass) return r;

    // 6. Margin
    r = check_margin(o, user, inst, mark_price);
    if (!r.pass) return r;

    // 7a. Reduce-only
    r = check_reduce_only(o, user);
    if (!r.pass) return r;

    // Check 7b (STP) is evaluated inside the matching engine.
    return {true, ""};
}

} // namespace asgard
