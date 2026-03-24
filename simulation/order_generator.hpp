#pragma once

#include "../engine/models.hpp"
#include "user_profiles.hpp"
#include <random>
#include <string>

namespace asgard::sim {

// ---------------------------------------------------------------------------
// OrderGenerator
//
// Generates realistic orders from a user profile + current market price.
// One instance per simulation thread (not thread-safe).
// ---------------------------------------------------------------------------

class OrderGenerator {
public:
    explicit OrderGenerator(uint64_t seed = std::random_device{}())
        : rng_(seed)
    {}

    // Generate a single order for a user given the market price.
    // Returns one or two orders (MM generates a bid+ask pair; returned as 1 or 2 orders)
    // The instrument name and user_id are embedded in the returned order.
    std::vector<Order> generate(const UserAccount& user,
                                 const UserProfile& profile,
                                 const std::string& instrument,
                                 double market_price,
                                 uint64_t order_seq_hint = 0);

    // Sample inter-arrival time (seconds) from a Poisson process
    double inter_arrival(double lambda);

private:
    // Order type selection
    OrderType sample_order_type(const UserProfile& profile);

    // Price generation
    double sample_limit_price(OrderSide side,
                               double market_price,
                               const UserProfile& profile);

    // Quantity generation (exponential, capped)
    double sample_quantity(const UserProfile& profile);

    // Generate a unique order ID
    std::string gen_order_id();

    std::mt19937_64 rng_;
    std::uniform_real_distribution<double> uniform_{0.0, 1.0};
    std::exponential_distribution<double>  exp_dist_{1.0};
    std::normal_distribution<double>       normal_{0.0, 1.0};
    uint64_t id_counter_ = 0;
};

} // namespace asgard::sim
