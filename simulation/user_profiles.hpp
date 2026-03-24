#pragma once

#include "../engine/models.hpp"
#include <string>

namespace asgard::sim {

// ---------------------------------------------------------------------------
// UserProfile  — configuration for a simulated user type
// ---------------------------------------------------------------------------

struct UserProfile {
    UserType    type;
    std::string name;

    // Order arrival rate (Poisson λ, orders per second)
    double arrival_rate;

    // Wallet balance range [min, max] — log-normal sampling
    double wallet_min;
    double wallet_max;

    // Rate limit (messages per second)
    int rate_limit;

    // Order type probabilities (must sum to ~1.0)
    double prob_limit;
    double prob_market;
    double prob_stop;

    // Price offset from market price (absolute fraction, e.g. 0.005 = 0.5%)
    double price_offset_max; // uniform [0, offset_max]

    // Quantity range [min, max] — exponential sampling
    double qty_mean;  // mean quantity
    double qty_max;   // hard cap

    // Spread for market makers (fraction of price, e.g. 0.0002 = 0.02%)
    double spread;
};

// ---------------------------------------------------------------------------
// Predefined profiles
// ---------------------------------------------------------------------------

inline UserProfile retail_profile() {
    return UserProfile{
        .type           = UserType::RETAIL,
        .name           = "RETAIL",
        .arrival_rate   = 0.05,       // 1 order per 20 sec
        .wallet_min     = 1'000.0,
        .wallet_max     = 50'000.0,
        .rate_limit     = 10,
        .prob_limit     = 0.70,
        .prob_market    = 0.25,
        .prob_stop      = 0.05,
        .price_offset_max = 0.015,    // up to ±1.5% from market
        .qty_mean       = 0.05,       // 0.05 BTC average
        .qty_max        = 0.5,
        .spread         = 0.0
    };
}

inline UserProfile algo_profile() {
    return UserProfile{
        .type           = UserType::ALGO,
        .name           = "ALGO",
        .arrival_rate   = 0.5,        // 1 order per 2 sec
        .wallet_min     = 10'000.0,
        .wallet_max     = 200'000.0,
        .rate_limit     = 50,
        .prob_limit     = 0.50,
        .prob_market    = 0.40,
        .prob_stop      = 0.10,
        .price_offset_max = 0.005,    // up to ±0.5% from market
        .qty_mean       = 0.2,
        .qty_max        = 2.0,
        .spread         = 0.0
    };
}

inline UserProfile mm_profile() {
    return UserProfile{
        .type           = UserType::MARKET_MAKER,
        .name           = "MM",
        .arrival_rate   = 5.0,        // 5 order pairs per sec
        .wallet_min     = 100'000.0,
        .wallet_max     = 1'000'000.0,
        .rate_limit     = 300,
        .prob_limit     = 0.99,
        .prob_market    = 0.01,
        .prob_stop      = 0.00,
        .price_offset_max = 0.0,      // not used for MM; spread drives pricing
        .qty_mean       = 2.0,        // 2 BTC average per quote side
        .qty_max        = 20.0,
        .spread         = 0.0002      // 2bps spread around mid
    };
}

} // namespace asgard::sim
