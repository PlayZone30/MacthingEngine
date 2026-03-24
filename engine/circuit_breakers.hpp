#pragma once

#include "models.hpp"
#include <deque>
#include <unordered_map>
#include <string>
#include <vector>
#include <cstdint>
#include <functional>

namespace asgard {

// ---------------------------------------------------------------------------
// VelocityDetector  — Layer 3 circuit breaker
//
// Maintains a rolling window of mark prices.
// Triggers when the endpoint price change exceeds threshold% in window_s.
// ---------------------------------------------------------------------------

class VelocityDetector {
public:
    VelocityDetector(double threshold_pct, int window_s)
        : threshold_(threshold_pct), window_s_(window_s) {}

    // Returns true if velocity threshold is exceeded (trigger cooldown).
    bool update(double mark_price, int64_t now_s);

    void reset() { history_.clear(); }

private:
    double threshold_;
    int    window_s_;
    std::deque<std::pair<int64_t, double>> history_; // (timestamp_s, price)
};

// ---------------------------------------------------------------------------
// CircuitBreakerManager  — manages all 4 layers across instruments
// ---------------------------------------------------------------------------

class CircuitBreakerManager {
public:
    using EmergencyCallback = std::function<void(const std::string& reason)>;

    void set_emergency_callback(EmergencyCallback cb) { on_emergency_ = std::move(cb); }

    // Call on every mark price update (per instrument).
    // Returns the new instrument state.
    InstrumentState update(Instrument& inst,
                            double mark_price,
                            const InsuranceVault& vault,
                            int64_t now_s);

    // Admin: force instrument into EMERGENCY
    void trigger_emergency(Instrument& inst, const std::string& reason);

    // Admin: recover from EMERGENCY → TRADING
    void recover(Instrument& inst);

    // Check if a market-wide emergency should be triggered based on
    // how many instruments are currently in COOLDOWN.
    void check_market_wide(std::vector<Instrument*>& instruments,
                            const InsuranceVault& vault);

    int total_cooldown_triggers()  const { return cooldown_count_; }
    int total_emergency_triggers() const { return emergency_count_; }

private:
    std::unordered_map<std::string, VelocityDetector> detectors_;
    std::unordered_map<std::string, int64_t> cooldown_expires_; // instrument → epoch_s
    EmergencyCallback on_emergency_;
    int cooldown_count_  = 0;
    int emergency_count_ = 0;
};

} // namespace asgard
