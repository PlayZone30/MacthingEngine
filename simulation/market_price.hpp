#pragma once

#include <random>
#include <cmath>

namespace asgard::sim {

// ---------------------------------------------------------------------------
// GBMSimulator  — Geometric Brownian Motion price simulator
//
// dS = μ·S·dt + σ·S·dW
//
// Default parameters match BTC-like behaviour:
//   S₀ = 50,000
//   μ  = 0.0 (no drift for stress-testing)
//   σ  = 0.60 (60% annual vol ≈ 0.038%/100ms tick)
//   dt = 0.1 s (100ms ticks → 10 ticks/sec)
// ---------------------------------------------------------------------------

class GBMSimulator {
public:
    GBMSimulator(double initial_price = 50'000.0,
                 double mu            = 0.0,
                 double sigma         = 0.60,
                 double dt            = 0.1)
        : price_(initial_price)
        , mu_(mu)
        , sigma_(sigma)
        , dt_(dt)
        , rng_(std::random_device{}())
        , dist_(0.0, 1.0)
    {}

    // Advance one tick and return the new price
    double tick() {
        double z = dist_(rng_);
        price_ *= std::exp((mu_ - 0.5 * sigma_ * sigma_) * dt_
                          + sigma_ * std::sqrt(dt_) * z);
        if (price_ < 1.0) price_ = 1.0; // floor
        return price_;
    }

    double price() const { return price_; }

    // Simulate N steps without recording — useful for warm-up
    void warmup(int steps) {
        for (int i = 0; i < steps; ++i) tick();
    }

private:
    double price_;
    double mu_, sigma_, dt_;
    std::mt19937_64 rng_;
    std::normal_distribution<double> dist_;
};

} // namespace asgard::sim
