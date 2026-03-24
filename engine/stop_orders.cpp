#include "stop_orders.hpp"
#include <algorithm>

namespace asgard {

void StopTriggerQueue::add(const StopOrder& s) {
    stops_.push_back(s);
}

bool StopTriggerQueue::remove(const std::string& order_id) {
    auto it = std::find_if(stops_.begin(), stops_.end(),
        [&](const StopOrder& s){ return s.order.order_id == order_id; });
    if (it == stops_.end()) return false;
    stops_.erase(it);
    return true;
}

void StopTriggerQueue::on_price_update(double mark_price,
                                        double last_traded_price,
                                        double index_price,
                                        const Instrument& inst,
                                        std::function<void(Order)> inject) {
    // Frozen during COOLDOWN: stops do not trigger
    if (inst.state == InstrumentState::COOLDOWN ||
        inst.state == InstrumentState::EMERGENCY) {
        return;
    }

    std::vector<StopOrder*> to_trigger;

    for (auto& s : stops_) {
        double ref_price;
        switch (s.trigger_source) {
            case TriggerSource::MARK:         ref_price = mark_price;         break;
            case TriggerSource::LAST_TRADED:  ref_price = last_traded_price;  break;
            case TriggerSource::INDEX:        ref_price = index_price;        break;
            default:                          ref_price = mark_price;         break;
        }

        bool fires = false;
        if (s.order.side == OrderSide::BUY  && ref_price >= s.trigger_price - 1e-9) fires = true;
        if (s.order.side == OrderSide::SELL && ref_price <= s.trigger_price + 1e-9) fires = true;

        if (fires && !s.triggered) {
            s.triggered = true;
            to_trigger.push_back(&s);
        }
    }

    // Inject triggered stops into the pipeline
    for (auto* s : to_trigger) {
        inject(s->to_limit_order());
    }

    // Remove triggered stops
    stops_.erase(
        std::remove_if(stops_.begin(), stops_.end(),
            [](const StopOrder& s){ return s.triggered; }),
        stops_.end());
}

std::vector<StopOrder> StopTriggerQueue::cancel_user(const std::string& user_id) {
    std::vector<StopOrder> cancelled;
    auto new_end = std::remove_if(stops_.begin(), stops_.end(),
        [&](const StopOrder& s) {
            if (s.user_id == user_id) {
                cancelled.push_back(s);
                return true;
            }
            return false;
        });
    stops_.erase(new_end, stops_.end());
    return cancelled;
}

} // namespace asgard
