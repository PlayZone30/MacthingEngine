#include "circuit_breakers.hpp"
#include <cmath>
#include <algorithm>

namespace asgard {

// ---------------------------------------------------------------------------
// VelocityDetector
// ---------------------------------------------------------------------------

bool VelocityDetector::update(double mark_price, int64_t now_s) {
    history_.push_back({now_s, mark_price});

    // Prune entries older than the window
    while (!history_.empty() && history_.front().first < now_s - window_s_)
        history_.pop_front();

    if (history_.size() < 2) return false;

    double oldest = history_.front().second;
    if (oldest <= 0.0) return false;

    double move = std::abs(mark_price - oldest) / oldest;
    return move > threshold_;
}

// ---------------------------------------------------------------------------
// CircuitBreakerManager::update
// ---------------------------------------------------------------------------

InstrumentState CircuitBreakerManager::update(Instrument& inst,
                                                double mark_price,
                                                const InsuranceVault& vault,
                                                int64_t now_s) {
    // Only apply circuit breaker logic when in TRADING or COOLDOWN
    if (inst.state == InstrumentState::EMERGENCY ||
        inst.state == InstrumentState::SETTLING  ||
        inst.state == InstrumentState::SETTLED)
        return inst.state;

    // ---------- Layer 3: Velocity detection --------------------------------
    auto& det = [&]() -> VelocityDetector& {
        auto it = detectors_.find(inst.symbol);
        if (it == detectors_.end()) {
            detectors_.emplace(inst.symbol,
                VelocityDetector(inst.velocity_threshold, inst.velocity_window_s));
            return detectors_.at(inst.symbol);
        }
        return it->second;
    }();

    if (inst.state == InstrumentState::TRADING) {
        bool triggered = det.update(mark_price, now_s);
        if (triggered) {
            inst.state = InstrumentState::COOLDOWN;
            cooldown_expires_[inst.symbol] = now_s + inst.cooldown_duration_s;
            ++cooldown_count_;
        }
    } else if (inst.state == InstrumentState::COOLDOWN) {
        det.update(mark_price, now_s); // keep history updated
        // Check if cooldown has expired
        auto ex_it = cooldown_expires_.find(inst.symbol);
        if (ex_it != cooldown_expires_.end() && now_s >= ex_it->second) {
            inst.state = InstrumentState::TRADING;
            det.reset();
        }
    }

    // ---------- Layer 4: Insurance vault stress ----------------------------
    if (vault.utilization() > 0.90) {
        if (inst.state != InstrumentState::EMERGENCY) {
            trigger_emergency(inst, "Insurance vault utilization > 90%");
        }
    }

    return inst.state;
}

// ---------------------------------------------------------------------------
// trigger_emergency
// ---------------------------------------------------------------------------

void CircuitBreakerManager::trigger_emergency(Instrument& inst,
                                               const std::string& reason) {
    inst.state = InstrumentState::EMERGENCY;
    ++emergency_count_;
    if (on_emergency_) on_emergency_(reason + " [" + inst.symbol + "]");
}

// ---------------------------------------------------------------------------
// recover
// ---------------------------------------------------------------------------

void CircuitBreakerManager::recover(Instrument& inst) {
    if (inst.state == InstrumentState::EMERGENCY) {
        inst.state = InstrumentState::TRADING;
        auto it = detectors_.find(inst.symbol);
        if (it != detectors_.end()) it->second.reset();
    }
}

// ---------------------------------------------------------------------------
// check_market_wide
// ---------------------------------------------------------------------------

void CircuitBreakerManager::check_market_wide(std::vector<Instrument*>& instruments,
                                               const InsuranceVault& vault) {
    if (instruments.empty()) return;

    int cooldowns = 0;
    for (auto* inst : instruments) {
        if (inst->state == InstrumentState::COOLDOWN) ++cooldowns;
    }

    // If ALL instruments are in cooldown simultaneously → systemic event
    bool all_cooldown = (cooldowns == static_cast<int>(instruments.size()));
    bool vault_stress = (vault.utilization() > 0.90);

    if (all_cooldown || vault_stress) {
        for (auto* inst : instruments) {
            if (inst->state != InstrumentState::EMERGENCY) {
                std::string reason = all_cooldown
                    ? "All instruments in COOLDOWN (systemic event)"
                    : "Insurance vault utilization > 90%";
                trigger_emergency(*inst, reason);
            }
        }
    }
}

} // namespace asgard
