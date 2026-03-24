#pragma once

#include "models.hpp"
#include <vector>
#include <functional>
#include <string>

namespace asgard {

// ---------------------------------------------------------------------------
// StopOrder  — a sleeping order waiting for a price trigger
// ---------------------------------------------------------------------------

struct StopOrder {
    Order          order;          // The limit order to inject when triggered
    double         trigger_price;
    TriggerSource  trigger_source  = TriggerSource::MARK;
    std::string    user_id;
    bool           triggered       = false;

    // Convert to the limit order that will be injected into the pipeline
    Order to_limit_order() const {
        Order lo       = order;
        lo.type        = OrderType::LIMIT;
        lo.price       = order.limit_price > 0.0 ? order.limit_price : order.trigger_price;
        lo.status      = OrderStatus::NEW;
        lo.timestamp_us = now_us();
        return lo;
    }
};

// ---------------------------------------------------------------------------
// StopTriggerQueue
//
// Scanned on every mark_price update.
// Frozen during COOLDOWN.
// ---------------------------------------------------------------------------

class StopTriggerQueue {
public:
    // Add a stop order.  IM for non-reduce-only stops must already be reserved
    // by the caller before calling this.
    void add(const StopOrder& s);

    // Remove a stop by order_id (e.g., on cancel or liquidation).
    bool remove(const std::string& order_id);

    // Scan for triggers.  Calls inject() for each triggered stop.
    // inject: callback to push the limit order into the main pipeline.
    // last_traded_price and index_price are used for non-MARK trigger sources.
    void on_price_update(double mark_price,
                         double last_traded_price,
                         double index_price,
                         const Instrument& inst,
                         std::function<void(Order)> inject);

    std::size_t size() const { return stops_.size(); }

    // Cancel all stops for a given user (e.g., cancel-on-disconnect for MM)
    // Returns the list of cancelled stop orders.
    std::vector<StopOrder> cancel_user(const std::string& user_id);

private:
    std::vector<StopOrder> stops_;
};

} // namespace asgard
