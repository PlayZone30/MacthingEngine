#include "orderbook.hpp"
#include <sstream>
#include <algorithm>
#include <stdexcept>
#include <cmath>

namespace asgard {

// ---------------------------------------------------------------------------
// add_order
// ---------------------------------------------------------------------------

void OrderBook::add_order(const Order& o) {
    if (o.side == OrderSide::BUY) {
        bids_[o.price].orders.push_back(o);
    } else {
        asks_[o.price].orders.push_back(o);
    }
    order_index_[o.order_id] = {o.side, o.price};
}

// ---------------------------------------------------------------------------
// remove_order
// ---------------------------------------------------------------------------

bool OrderBook::remove_order(const std::string& order_id, double /*price*/, OrderSide /*side*/) {
    auto it = order_index_.find(order_id);
    if (it == order_index_.end()) return false;

    OrderSide s   = it->second.side;
    double    p   = it->second.price;
    order_index_.erase(it);

    auto remove_from_level = [&](auto& book_map) -> bool {
        auto lvl_it = book_map.find(p);
        if (lvl_it == book_map.end()) return false;
        auto& dq = lvl_it->second.orders;
        auto order_it = std::find_if(dq.begin(), dq.end(),
            [&](const Order& ord){ return ord.order_id == order_id; });
        if (order_it == dq.end()) return false;
        dq.erase(order_it);
        if (dq.empty()) book_map.erase(lvl_it);
        return true;
    };

    if (s == OrderSide::BUY)  return remove_from_level(bids_);
    else                       return remove_from_level(asks_);
}

// ---------------------------------------------------------------------------
// peek_front  — returns pointer into the deque (valid until next mutation)
// ---------------------------------------------------------------------------

Order* OrderBook::peek_front(OrderSide side, double price) {
    if (side == OrderSide::BUY) {
        // peek the ask side (the resting order is on the opposite side)
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
// consume_front
// ---------------------------------------------------------------------------

void OrderBook::consume_front(OrderSide side, double price, double fill_qty) {
    auto do_consume = [&](auto& book_map) {
        auto it = book_map.find(price);
        if (it == book_map.end()) return;
        auto& dq = it->second.orders;
        if (dq.empty()) return;

        Order& front = dq.front();
        front.remaining_qty -= fill_qty;

        if (front.remaining_qty < 1e-9) {
            order_index_.erase(front.order_id);
            dq.pop_front();
            if (dq.empty()) book_map.erase(it);
        } else {
            // Partial fill: update status in index (order still resting)
            front.status = OrderStatus::PARTIAL;
        }
    };

    // "side" is the AGGRESSOR side.  The resting order is on the opposite book.
    if (side == OrderSide::BUY)  do_consume(asks_);
    else                          do_consume(bids_);
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
// best_bid / best_ask
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
// ---------------------------------------------------------------------------

double OrderBook::available_qty(OrderSide aggressive_side, double limit_price) const {
    double total = 0.0;

    if (aggressive_side == OrderSide::BUY) {
        // BUY order: walk the ask side, levels ≤ limit_price
        for (auto& [p, lvl] : asks_) {
            if (p > limit_price + 1e-9) break; // asks sorted asc; stop when too high
            total += lvl.total_qty();
        }
    } else {
        // SELL order: walk the bid side, levels ≥ limit_price
        for (auto& [p, lvl] : bids_) {
            if (p < limit_price - 1e-9) break; // bids sorted desc; stop when too low
            total += lvl.total_qty();
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
        oss << "[" << p << "," << lvl.total_qty() << "]";
        ++cnt;
    }
    oss << "],\"asks\":[";
    cnt = 0;
    for (auto& [p, lvl] : asks_) {
        if (cnt >= depth) break;
        if (cnt > 0) oss << ",";
        oss << "[" << p << "," << lvl.total_qty() << "]";
        ++cnt;
    }
    oss << "],\"seq\":" << seq << "}";
    return oss.str();
}

// ---------------------------------------------------------------------------
// total_bid_qty / total_ask_qty  (for tests / stats)
// ---------------------------------------------------------------------------

double OrderBook::total_bid_qty() const {
    double t = 0;
    for (auto& [p, lvl] : bids_) t += lvl.total_qty();
    return t;
}

double OrderBook::total_ask_qty() const {
    double t = 0;
    for (auto& [p, lvl] : asks_) t += lvl.total_qty();
    return t;
}

} // namespace asgard
