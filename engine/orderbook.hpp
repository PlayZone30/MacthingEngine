#pragma once

#include "models.hpp"
#include <map>
#include <deque>
#include <optional>
#include <functional>
#include <string>
#include <unordered_map>

// nlohmann/json forward inclusion handled in .cpp

namespace asgard {

// ---------------------------------------------------------------------------
// PriceLevel  — FIFO deque of orders at one price
// ---------------------------------------------------------------------------

struct PriceLevel {
    std::deque<Order> orders;

    double total_qty() const {
        double q = 0.0;
        for (auto& o : orders) q += o.remaining_qty;
        return q;
    }
};

// ---------------------------------------------------------------------------
// OrderBook  — FIFO CLOB
//
// Bids:  std::map<price, PriceLevel, std::greater<double>>  → begin() = best bid
// Asks:  std::map<price, PriceLevel>                        → begin() = best ask
// ---------------------------------------------------------------------------

class OrderBook {
public:
    // --- Mutating operations -----------------------------------------------

    // Add a resting order.  Appends to the back of the deque at its price
    // level (FIFO: new orders go to back; fills come from front).
    void add_order(const Order& o);

    // Remove a specific order (by order_id) from its price level.
    // Returns true if found and removed.
    bool remove_order(const std::string& order_id, double price, OrderSide side);

    // Remove the front of a level by consuming qty.
    // If the front order is fully consumed, it is erased.
    // Returns the order (possibly partially remaining) that was at the front.
    Order* peek_front(OrderSide side, double price);

    // Consume fill_qty from the front of the level at price.
    // Erases the front order when its remaining_qty reaches 0.
    void consume_front(OrderSide side, double price, double fill_qty);

    // Erase an empty price level.
    void prune_empty(OrderSide side, double price);

    // --- Read-only queries --------------------------------------------------

    std::optional<double> best_bid() const;
    std::optional<double> best_ask() const;
    double mid_price() const;

    // Total available qty at-or-better than limit_price on the aggressive side.
    // Used by FOK pre-check.  aggressive_side = side of the incoming order.
    double available_qty(OrderSide aggressive_side, double limit_price) const;

    // Check if an incoming order would immediately cross (used by POST_ONLY).
    bool would_cross(const Order& incoming) const;

    // L2 depth snapshot  [price, qty] arrays for bids and asks
    // Returns json object: { "bids": [[p,q],...], "asks": [[p,q],...], "seq": N }
    std::string snapshot_json(int depth, uint64_t seq) const;

    // Number of resting orders
    std::size_t order_count() const { return order_index_.size(); }

    // --- Debug / test helpers -----------------------------------------------
    // Return total qty resting on bid side
    double total_bid_qty() const;
    double total_ask_qty() const;

private:
    std::map<double, PriceLevel, std::greater<double>> bids_; // desc → begin()=best
    std::map<double, PriceLevel>                        asks_; // asc  → begin()=best

    // order_id → (side, price) for O(1) lookup on cancel
    struct OrderRef { OrderSide side; double price; };
    std::unordered_map<std::string, OrderRef> order_index_;
};

} // namespace asgard
