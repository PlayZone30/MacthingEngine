#include "orderbook.hpp"
#include <sstream>
#include <cmath>

namespace asgard {

// ---------------------------------------------------------------------------
// Thread-local pool resource
//
// A single unsynchronized_pool_resource per engine thread.  All LevelLists
// on this thread draw from it, avoiding per-node calls to the global
// allocator (malloc/free).  unsynchronized_pool_resource is correct here
// because the matching engine is strictly single-threaded.
//
// Defined in the .cpp so the pool lifetime is tied to this translation unit
// and is not accidentally shared across compilation units.
// ---------------------------------------------------------------------------

static thread_local std::pmr::unsynchronized_pool_resource tl_pool;

std::pmr::memory_resource* level_list_pool() { return &tl_pool; }

// ---------------------------------------------------------------------------
// PriceLevel default constructor — uses the thread-local pool
// ---------------------------------------------------------------------------

PriceLevel::PriceLevel() : orders(level_list_pool()) {}

// ---------------------------------------------------------------------------
// OrderBook constructor
// ---------------------------------------------------------------------------

OrderBook::OrderBook(std::size_t expected_orders) {
    // Pre-bucket the order index to avoid rehash spikes during warm-up.
    order_index_.reserve(expected_orders);
}

// ---------------------------------------------------------------------------
// add_order
//
// Appends the order at the FIFO tail of its price level.
// Stores a direct LevelList iterator in order_index_ for O(1) cancel.
// ---------------------------------------------------------------------------

void OrderBook::add_order(const Order& o) {
    if (o.side == OrderSide::BUY) {
        // map::operator[] default-constructs a PriceLevel (via PriceLevel())
        // if the key is absent; otherwise returns the existing level.
        PriceLevel& lvl  = bids_[o.price];
        auto        it   = lvl.orders.insert(lvl.orders.end(), o);
        lvl.qty         += o.remaining_qty;
        order_index_.emplace(o.order_id, OrderRef{o.side, o.price, it});
    } else {
        PriceLevel& lvl  = asks_[o.price];
        auto        it   = lvl.orders.insert(lvl.orders.end(), o);
        lvl.qty         += o.remaining_qty;
        order_index_.emplace(o.order_id, OrderRef{o.side, o.price, it});
    }
}

// ---------------------------------------------------------------------------
// remove_order  — O(1) cancel via stored iterator
//
//  1. order_index_.find()       → O(1) hash lookup
//  2. book_map.find(price)      → O(log N) (the map lookup; infrequent)
//  3. lvl.orders.erase(iter)    → O(1) list pointer rewire — no linear scan
//  4. Prune empty level         → O(log N) map::erase (only when level empties)
//  5. order_index_.erase()      → O(1)
// ---------------------------------------------------------------------------

bool OrderBook::remove_order(const std::string& order_id,
                              double /*price*/, OrderSide /*side*/) {
    auto idx_it = order_index_.find(order_id);
    if (idx_it == order_index_.end()) return false;

    const OrderRef& ref = idx_it->second;

    if (ref.side == OrderSide::BUY) {
        auto lvl_it = bids_.find(ref.price);
        if (lvl_it != bids_.end()) {
            PriceLevel& lvl = lvl_it->second;
            lvl.qty -= ref.iter->remaining_qty;
            lvl.orders.erase(ref.iter);           // O(1)
            if (lvl.orders.empty()) bids_.erase(lvl_it);
        }
    } else {
        auto lvl_it = asks_.find(ref.price);
        if (lvl_it != asks_.end()) {
            PriceLevel& lvl = lvl_it->second;
            lvl.qty -= ref.iter->remaining_qty;
            lvl.orders.erase(ref.iter);           // O(1)
            if (lvl.orders.empty()) asks_.erase(lvl_it);
        }
    }

    order_index_.erase(idx_it);
    return true;
}

// ---------------------------------------------------------------------------
// do_consume_front  — shared template used by the compat consume_front path
//
// Reduces remaining_qty on the front order.  On full fill:
//   • erases from order_index_
//   • pops from the list (deallocates node back to pool)
//   • erases the price level if empty
// ---------------------------------------------------------------------------

template<typename MapT>
void OrderBook::do_consume_front(MapT& book_map,
                                  typename MapT::iterator lvl_it,
                                  double fill_qty) {
    PriceLevel& lvl   = lvl_it->second;
    Order&      front = lvl.orders.front();

    front.remaining_qty -= fill_qty;
    lvl.qty             -= fill_qty;

    if (front.remaining_qty < 1e-9) {
        order_index_.erase(front.order_id);
        front.status = OrderStatus::FILLED;
        lvl.orders.pop_front();
        if (lvl.orders.empty()) book_map.erase(lvl_it);
    } else {
        front.status = OrderStatus::PARTIAL;
    }
}

// ---------------------------------------------------------------------------
// peek_front  — retained for test compatibility
// ---------------------------------------------------------------------------

Order* OrderBook::peek_front(OrderSide aggressor_side, double price) {
    if (aggressor_side == OrderSide::BUY) {
        auto it = asks_.find(price);
        if (it == asks_.end() || it->second.orders.empty()) return nullptr;
        return &it->second.orders.front();
    } else {
        auto it = bids_.find(price);
        if (it == bids_.end() || it->second.orders.empty()) return nullptr;
        return &it->second.orders.front();
    }
}

// ---------------------------------------------------------------------------
// consume_front  — retained for test compatibility
// ---------------------------------------------------------------------------

void OrderBook::consume_front(OrderSide aggressor_side, double price, double fill_qty) {
    if (aggressor_side == OrderSide::BUY) {
        auto it = asks_.find(price);
        if (it != asks_.end()) do_consume_front(asks_, it, fill_qty);
    } else {
        auto it = bids_.find(price);
        if (it != bids_.end()) do_consume_front(bids_, it, fill_qty);
    }
}

// ---------------------------------------------------------------------------
// best_level_ptr  — hot-path helper
//
// Returns (price, PriceLevel*) in a single map::begin() call, avoiding the
// redundant map::find that would follow a best_ask()/best_bid() call.
// ---------------------------------------------------------------------------

PriceLevel* OrderBook::best_level_ptr(OrderSide aggressor_side, double& out_price) {
    if (aggressor_side == OrderSide::BUY) {
        if (asks_.empty()) return nullptr;
        auto it   = asks_.begin();
        out_price = it->first;
        return &it->second;
    } else {
        if (bids_.empty()) return nullptr;
        auto it   = bids_.begin();
        out_price = it->first;
        return &it->second;
    }
}

// ---------------------------------------------------------------------------
// consume_level_front  — hot-path helper
//
// `lvl` MUST be the current begin() level on the opposite side.
// On level exhaustion uses erase(begin()) — O(1) amortized — instead of
// find(price)+erase, saving one O(log N) lookup per level exhaustion.
// For partial fills: no map operation at all.
// ---------------------------------------------------------------------------

void OrderBook::consume_level_front(OrderSide aggressor_side,
                                     PriceLevel* lvl, double fill_qty) {
    Order& front = lvl->orders.front();

    front.remaining_qty -= fill_qty;
    lvl->qty            -= fill_qty;

    if (front.remaining_qty < 1e-9) {
        order_index_.erase(front.order_id);
        front.status = OrderStatus::FILLED;
        lvl->orders.pop_front();
        if (lvl->orders.empty()) {
            // lvl is always the begin() level — erase(begin()) is O(1) amortized.
            if (aggressor_side == OrderSide::BUY)
                asks_.erase(asks_.begin());
            else
                bids_.erase(bids_.begin());
        }
    } else {
        front.status = OrderStatus::PARTIAL;
    }
}

// ---------------------------------------------------------------------------
// prune_empty
// ---------------------------------------------------------------------------

void OrderBook::prune_empty(OrderSide side, double price) {
    if (side == OrderSide::BUY) {
        auto it = bids_.find(price);
        if (it != bids_.end() && it->second.orders.empty()) bids_.erase(it);
    } else {
        auto it = asks_.find(price);
        if (it != asks_.end() && it->second.orders.empty()) asks_.erase(it);
    }
}

// ---------------------------------------------------------------------------
// best_bid / best_ask / mid_price
// ---------------------------------------------------------------------------

std::optional<double> OrderBook::best_bid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<double> OrderBook::best_ask() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

double OrderBook::mid_price() const {
    auto bb = best_bid();
    auto ba = best_ask();
    if (!bb && !ba) return 0.0;
    if (!bb) return *ba;
    if (!ba) return *bb;
    return (*bb + *ba) / 2.0;
}

// ---------------------------------------------------------------------------
// available_qty  — for FOK pre-check
//
// Uses incremental PriceLevel::qty — O(L) per call, not O(L×N).
// ---------------------------------------------------------------------------

double OrderBook::available_qty(OrderSide aggressive_side, double limit_price) const {
    double total = 0.0;

    if (aggressive_side == OrderSide::BUY) {
        for (auto& [p, lvl] : asks_) {
            if (p > limit_price + 1e-9) break;    // asks sorted asc
            total += lvl.qty;
        }
    } else {
        for (auto& [p, lvl] : bids_) {
            if (p < limit_price - 1e-9) break;    // bids sorted desc
            total += lvl.qty;
        }
    }
    return total;
}

// ---------------------------------------------------------------------------
// would_cross
// ---------------------------------------------------------------------------

bool OrderBook::would_cross(const Order& incoming) const {
    if (incoming.side == OrderSide::BUY) {
        auto ba = best_ask();
        if (!ba) return false;
        if (incoming.type == OrderType::MARKET) return true;
        return incoming.price >= *ba;
    } else {
        auto bb = best_bid();
        if (!bb) return false;
        if (incoming.type == OrderType::MARKET) return true;
        return incoming.price <= *bb;
    }
}

// ---------------------------------------------------------------------------
// snapshot_json
// ---------------------------------------------------------------------------

std::string OrderBook::snapshot_json(int depth, uint64_t seq) const {
    std::ostringstream oss;
    oss << "{\"bids\":[";
    int cnt = 0;
    for (auto& [p, lvl] : bids_) {
        if (cnt >= depth) break;
        if (cnt > 0) oss << ",";
        oss << "[" << p << "," << lvl.qty << "]";
        ++cnt;
    }
    oss << "],\"asks\":[";
    cnt = 0;
    for (auto& [p, lvl] : asks_) {
        if (cnt >= depth) break;
        if (cnt > 0) oss << ",";
        oss << "[" << p << "," << lvl.qty << "]";
        ++cnt;
    }
    oss << "],\"seq\":" << seq << "}";
    return oss.str();
}

// ---------------------------------------------------------------------------
// total_bid_qty / total_ask_qty  — debug / test helpers
// ---------------------------------------------------------------------------

double OrderBook::total_bid_qty() const {
    double t = 0.0;
    for (auto& [p, lvl] : bids_) t += lvl.qty;
    return t;
}

double OrderBook::total_ask_qty() const {
    double t = 0.0;
    for (auto& [p, lvl] : asks_) t += lvl.qty;
    return t;
}

} // namespace asgard
