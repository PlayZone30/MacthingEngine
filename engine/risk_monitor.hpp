#pragma once

#include "models.hpp"
#include "margin.hpp"
#include <functional>
#include <string>
#include <vector>

namespace asgard {

// ---------------------------------------------------------------------------
// RiskMonitor
//
// Called on every mark price update.
// Checks each user's equity against maintenance margin.
// Fires the liquidation callback when equity ≤ MM.
// Fires the margin_warning callback when equity ≤ IM × 1.1.
// ---------------------------------------------------------------------------

class RiskMonitor {
public:
    // Callback types
    using LiquidationCallback = std::function<void(const std::string& user_id,
                                                    const std::string& instrument)>;
    using WarningCallback     = std::function<void(const std::string& user_id,
                                                    double equity,
                                                    double total_im)>;

    void set_liquidation_callback(LiquidationCallback cb) { on_liq_ = std::move(cb); }
    void set_warning_callback(WarningCallback cb)         { on_warn_ = std::move(cb); }

    // Scan all users with open positions.  Call from the mark-price tick.
    void scan(std::vector<UserAccount*>& users,
              double mark_price,
              const Instrument& inst);

private:
    LiquidationCallback on_liq_;
    WarningCallback     on_warn_;
};

} // namespace asgard
