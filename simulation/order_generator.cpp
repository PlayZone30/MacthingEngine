#include "order_generator.hpp"
#include <cmath>
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace asgard::sim {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string OrderGenerator::gen_order_id() {
    std::ostringstream oss;
    oss << "O-" << std::setfill('0') << std::setw(10) << (++id_counter_);
    return oss.str();
}

double OrderGenerator::inter_arrival(double lambda) {
    // Exponential inter-arrival time for Poisson process
    std::exponential_distribution<double> d(lambda);
    return d(rng_);
}

OrderType OrderGenerator::sample_order_type(const UserProfile& profile) {
    double r = uniform_(rng_);
    if (r < profile.prob_limit)  return OrderType::LIMIT;
    if (r < profile.prob_limit + profile.prob_market) return OrderType::MARKET;
    return OrderType::STOP_LIMIT;
}

double OrderGenerator::sample_limit_price(OrderSide side,
                                           double market_price,
                                           const UserProfile& profile) {
    // Uniform offset in [0, offset_max]
    double offset_frac = uniform_(rng_) * profile.price_offset_max;

    // Round to a sensible number of decimal places (simulate price tick of $1)
    double raw_price;
    if (side == OrderSide::BUY) {
        raw_price = market_price * (1.0 - offset_frac);
    } else {
        raw_price = market_price * (1.0 + offset_frac);
    }
    // Round to nearest $0.5 for realism
    return std::round(raw_price * 2.0) / 2.0;
}

double OrderGenerator::sample_quantity(const UserProfile& profile) {
    // Exponential distribution: most orders small, few large
    std::exponential_distribution<double> d(1.0 / profile.qty_mean);
    double qty = d(rng_);
    qty = std::max(0.001, std::min(qty, profile.qty_max));
    // Round to nearest 0.001 (lot step)
    return std::round(qty * 1000.0) / 1000.0;
}

// ---------------------------------------------------------------------------
// generate
// ---------------------------------------------------------------------------

std::vector<Order> OrderGenerator::generate(const UserAccount& user,
                                              const UserProfile& profile,
                                              const std::string& instrument,
                                              double market_price,
                                              uint64_t /*order_seq_hint*/) {
    std::vector<Order> result;

    if (profile.type == UserType::MARKET_MAKER) {
        // MMs place both a bid and an ask simultaneously
        double half_spread = market_price * profile.spread / 2.0;
        double bid_price   = std::round((market_price - half_spread) * 2.0) / 2.0;
        double ask_price   = std::round((market_price + half_spread) * 2.0) / 2.0;
        double qty         = sample_quantity(profile);

        // Bid
        Order bid;
        bid.order_id     = gen_order_id();
        bid.user_id      = user.user_id;
        bid.instrument   = instrument;
        bid.side         = OrderSide::BUY;
        bid.type         = OrderType::LIMIT;  // MMs use POST_ONLY in production
        bid.price        = bid_price;
        bid.quantity     = qty;
        bid.remaining_qty = qty;
        bid.tif          = TIF::GTC;
        bid.stp_mode     = STPMode::CANCEL_RESTING; // MMs cancel resting on self-trade
        bid.timestamp_us = now_us();
        bid.status       = OrderStatus::NEW;
        result.push_back(bid);

        // Ask
        Order ask = bid;
        ask.order_id     = gen_order_id();
        ask.side         = OrderSide::SELL;
        ask.price        = ask_price;
        ask.timestamp_us = now_us();
        result.push_back(ask);

        return result;
    }

    // Non-MM: single order
    OrderSide side = (uniform_(rng_) < 0.5) ? OrderSide::BUY : OrderSide::SELL;
    OrderType type = sample_order_type(profile);
    double    qty  = sample_quantity(profile);
    double    price = 0.0;

    if (type == OrderType::LIMIT || type == OrderType::STOP_LIMIT) {
        price = sample_limit_price(side, market_price, profile);
    }

    Order o;
    o.order_id      = gen_order_id();
    o.user_id       = user.user_id;
    o.instrument    = instrument;
    o.side          = side;
    o.type          = (type == OrderType::STOP_LIMIT) ? OrderType::LIMIT : type; // simplified
    o.price         = price;
    o.quantity      = qty;
    o.remaining_qty = qty;
    o.tif           = (type == OrderType::MARKET) ? TIF::IOC : TIF::GTC;
    o.stp_mode      = STPMode::CANCEL_INCOMING;
    o.timestamp_us  = now_us();
    o.status        = OrderStatus::NEW;

    // 10% chance of reduce-only for existing position holders (simulates closing)
    if (uniform_(rng_) < 0.10) {
        auto pos_it = user.positions.find(instrument);
        if (pos_it != user.positions.end()) {
            auto& pos = pos_it->second;
            // Only set reduce-only if it would reduce (not increase) the position
            bool would_reduce = (side == OrderSide::SELL && pos.side == PositionSide::LONG) ||
                                 (side == OrderSide::BUY  && pos.side == PositionSide::SHORT);
            if (would_reduce) {
                o.reduce_only = true;
                o.quantity     = std::min(o.quantity, pos.size);
                o.remaining_qty = o.quantity;
            }
        }
    }

    result.push_back(o);
    return result;
}

} // namespace asgard::sim
