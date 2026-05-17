#pragma once

#include "models.hpp"
#include <list>
#include <map>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <unordered_map>

namespace asgard {

// ---------------------------------------------------------------------------
// LevelList — pmr-backed list for O(1) iterator stability + pool allocation.
//
// The pool is a thread_local unsynchronized_pool_resource defined in
// orderbook.cpp and shared by all OrderBook instances on the same thread.
// The matching engine is single-threaded, so no synchronisation is needed.
// ---------------------------------------------------------------------------
using LevelList = std::pmr::list<Order>;

// Forward declaration so PriceLevel can reference the pool accessor.
std::pmr::memory_resource* level_list_pool();

// ---------------------------------------------------------------------------
// PriceLevel  — FIFO list of resting orders at one price
//
//  • insert(end)   → O(1), appends at FIFO tail
//  • front()       → O(1), peeks FIFO head (next to fill)
//  • pop_front()   → O(1), removes FIFO head on full fill
//  • erase(iter)   → O(1), O(1) cancel via stored iterator
//  • qty           → maintained incrementally; no O(N) scan
// ---------------------------------------------------------------------------
struct PriceLevel {
    LevelList orders;
    double    qty = 0.0;   // total remaining qty, maintained incrementally

    // Default constructor uses the thread-local pool (engine thread only).
    PriceLevel();

    // std::map node operations need move semantics.
    PriceLevel(PriceLevel&&)            = default;
    PriceLevel& operator=(PriceLevel&&) = default;

    // Copying a PriceLevel is not meaningful (pool ownership unclear).
    PriceLevel(const PriceLevel&)            = delete;
    PriceLevel& operator=(const PriceLevel&) = delete;

    double total_qty() const { return qty; }   // kept for API compatibility
};

// ---------------------------------------------------------------------------
// OrderBook  — FIFO CLOB
//
//  Bids:  std::map<price, PriceLevel, std::greater<double>>  begin()=best bid
//  Asks:  std::map<price, PriceLevel>                         begin()=best ask
//
//  order_index_ maps order_id → OrderRef which holds a direct LevelList
//  iterator.  Cancel is O(1): list::erase(iter), no linear scan.
// ---------------------------------------------------------------------------
class OrderBook {
public:
    // expected_orders: pre-buckets the index to avoid rehash during warm-up.
    explicit OrderBook(std::size_t expected_orders = 65536);

    // --- Mutating operations -----------------------------------------------

    // Append a resting order at the FIFO tail of its price level.
    void add_order(const Order& o);

    // O(1) cancel using the iterator stored in order_index_.
    // price/side params retained for API compatibility but are unused.
    bool remove_order(const std::string& order_id,
                      double /*price*/, OrderSide /*side*/);

    // Returns a pointer to the FIFO front order at `price` on the opposite
    // side from `aggressor_side`.  Valid until the next mutation.
    // (Kept for test compatibility; matching hot path uses best_level_ptr.)
    Order* peek_front(OrderSide aggressor_side, double price);

    // Consume fill_qty from the FIFO front at `price` on the opposite side.
    // (Kept for test compatibility; matching hot path uses consume_level_front.)
    void consume_front(OrderSide aggressor_side, double price, double fill_qty);

    // Defensive clean-up of empty levels (normally auto-pruned inline).
    void prune_empty(OrderSide side, double price);

    // --- Hot-path helpers for the matching engine --------------------------
    //
    // best_level_ptr() returns (price, PriceLevel*) in a single map::begin()
    // call, avoiding the redundant map::find that would follow best_ask()/
    // best_bid() + peek_front(price).
    //
    // consume_level_front() operates on the known begin() level so it can
    // call erase(begin()) on exhaustion instead of find(price)+erase.

    // Return the best PriceLevel* and its price for the side opposite to
    // `aggressor_side`.  Returns nullptr when that side is empty.
    PriceLevel* best_level_ptr(OrderSide aggressor_side, double& out_price);

    // Consume fill_qty from the FIFO front of `lvl`, which MUST be the
    // current begin() level on the opposite side.
    void consume_level_front(OrderSide aggressor_side,
                             PriceLevel* lvl, double fill_qty);

    // --- Read-only queries --------------------------------------------------

    std::optional<double> best_bid() const;
    std::optional<double> best_ask() const;
    double mid_price() const;

    // Total qty at-or-better than limit_price on the opposite side.
    // Uses incremental PriceLevel::qty — O(L), not O(L×N).
    double available_qty(OrderSide aggressive_side, double limit_price) const;

    bool would_cross(const Order& incoming) const;

    // L2 depth snapshot as a JSON string.
    std::string snapshot_json(int depth, uint64_t seq) const;

    std::size_t order_count() const { return order_index_.size(); }

    // Debug / test helpers
    double total_bid_qty() const;
    double total_ask_qty() const;

private:
    std::map<double, PriceLevel, std::greater<double>> bids_;  // desc→begin()=best
    std::map<double, PriceLevel>                        asks_;  // asc →begin()=best

    // Stores a direct LevelList iterator per order for O(1) cancel.
    struct OrderRef {
        OrderSide           side;
        double              price;
        LevelList::iterator iter;   // O(1) erase: list::erase(iter)
    };
    std::unordered_map<std::string, OrderRef> order_index_;

    // Shared consume logic (templated on map comparator).
    template<typename MapT>
    void do_consume_front(MapT& book_map,
                          typename MapT::iterator lvl_it,
                          double fill_qty);
};

} // namespace asgard
