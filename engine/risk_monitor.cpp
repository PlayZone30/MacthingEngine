#include "risk_monitor.hpp"

namespace asgard {

void RiskMonitor::scan(std::vector<UserAccount*>& users,
                        double mark_price,
                        const Instrument& inst) {
    for (UserAccount* user : users) {
        if (user->positions.empty()) continue;

        // Cross-margin check
        double eq       = cross_equity(*user, mark_price);
        double total_mm = total_cross_mm(*user, mark_price, inst);
        double total_im = total_cross_im(*user, mark_price, inst);

        if (eq <= total_mm + 1e-9) {
            // Trigger liquidation for the worst position
            // Find position with largest unrealized loss
            std::string worst_instrument;
            double worst_upnl = 0.0;
            for (auto& [sym, pos] : user->positions) {
                if (pos.state == PositionState::LIQUIDATING) continue;
                double upnl = unrealized_pnl(pos, mark_price);
                if (upnl < worst_upnl) {
                    worst_upnl = upnl;
                    worst_instrument = sym;
                }
            }
            if (worst_instrument.empty() && !user->positions.empty()) {
                worst_instrument = user->positions.begin()->first;
            }
            if (!worst_instrument.empty() && on_liq_) {
                on_liq_(user->user_id, worst_instrument);
            }
        } else if (eq <= total_im * 1.1 + 1e-9 && on_warn_) {
            on_warn_(user->user_id, eq, total_im);
        }

        // Isolated-margin check for each position
        for (auto& [sym, pos] : user->positions) {
            if (pos.margin_mode != MarginMode::ISOLATED) continue;
            if (pos.state == PositionState::LIQUIDATING) continue;
            double upnl = unrealized_pnl(pos, mark_price);
            double eff_eq = pos.allocated_margin + upnl;
            double pos_mm = position_mm(pos, mark_price, inst);
            if (eff_eq <= pos_mm + 1e-9 && on_liq_) {
                on_liq_(user->user_id, sym);
            }
        }
    }
}

} // namespace asgard
