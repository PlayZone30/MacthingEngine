#pragma once

#include <algorithm>
#include <cmath>

namespace asgard {

// ---------------------------------------------------------------------------
// MarkPriceEngine
//
// Formula (adopted from Backpack):
//   mark_price = clamp(index_price + EWMA(mid - index_price), index×0.95, index×1.05)
//
// EWMA with 1-minute halflife, called once per tick.
// The caller determines the tick interval; alpha should be computed as:
//   alpha = 1 - exp(-ln(2) * dt_seconds / 60)
// For dt = 0.1s (100ms ticks): alpha ≈ 0.001155
// For per-second updates:       alpha ≈ 0.011513
// ---------------------------------------------------------------------------

class MarkPriceEngine {
public:
    // dt_seconds: how frequently update() is called (100ms = 0.1)
    explicit MarkPriceEngine(double dt_seconds = 0.1)
        : alpha_(1.0 - std::exp(-std::log(2.0) * dt_seconds / 60.0))
    {}

    // Call on every market data tick.
    // index_price: oracle price (in simulation: GBM price)
    // mid_price:   (best_bid + best_ask) / 2  (0 if book is empty)
    double update(double index_price, double mid_price) {
        if (index_price <= 0.0) return mark_;

        double basis = (mid_price > 0.0) ? (mid_price - index_price) : 0.0;
        ewma_ = alpha_ * basis + (1.0 - alpha_) * ewma_;

        double raw_mark = index_price + ewma_;
        mark_ = std::clamp(raw_mark, index_price * 0.95, index_price * 1.05);
        index_ = index_price;
        return mark_;
    }

    double mark()  const { return mark_; }
    double index() const { return index_; }
    double basis() const { return mark_ - index_; }

    void reset(double initial_price) {
        mark_  = initial_price;
        index_ = initial_price;
        ewma_  = 0.0;
    }

private:
    double alpha_;
    double ewma_  = 0.0;
    double mark_  = 0.0;
    double index_ = 0.0;
};

} // namespace asgard
