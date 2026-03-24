#include "stats_collector.hpp"
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace asgard::stats {

// ---------------------------------------------------------------------------
// Hot-path recording (called from engine thread)
// ---------------------------------------------------------------------------

void StatsCollector::record_order(const Order& /*o*/) {
    orders_.fetch_add(1, std::memory_order_relaxed);
}

void StatsCollector::record_trade(const Trade& t) {
    trades_.fetch_add(1, std::memory_order_relaxed);

    // Latency: time from order submission to trade
    uint64_t now = now_us();
    if (t.timestamp_us > 0 && now >= t.timestamp_us) {
        uint64_t latency = now - t.timestamp_us;
        std::lock_guard<std::mutex> lk(lat_mutex_);
        lat_samples_.push_back(latency);
    }
}

void StatsCollector::record_rejection(const std::string& reason) {
    rejections_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(rej_mutex_);
    rej_counts_[reason]++;
}

void StatsCollector::record_liquidation() {
    liquidations_.fetch_add(1, std::memory_order_relaxed);
}

void StatsCollector::update_pnl(const std::string& user_id, double realized_pnl) {
    std::lock_guard<std::mutex> lk(pnl_mutex_);
    cumulative_pnl_[user_id] += realized_pnl;
}

void StatsCollector::set_market_data(double last, double bid, double ask) {
    std::lock_guard<std::mutex> lk(mkt_mutex_);
    last_price_ = last;
    best_bid_   = bid;
    best_ask_   = ask;
}

void StatsCollector::set_vault(double balance, double utilization) {
    std::lock_guard<std::mutex> lk(mkt_mutex_);
    vault_balance_      = balance;
    vault_utilization_  = utilization;
}

void StatsCollector::set_zero_sum_error(double err) {
    std::lock_guard<std::mutex> lk(mkt_mutex_);
    zero_sum_error_ = err;
}

// ---------------------------------------------------------------------------
// snapshot — called every 10s from monitor thread
// ---------------------------------------------------------------------------

Snapshot StatsCollector::snapshot() {
    Snapshot s;

    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - last_snapshot_time_).count();
    if (elapsed < 1e-6) elapsed = 1.0;

    uint64_t cur_orders = orders_.load();
    uint64_t cur_trades = trades_.load();
    s.total_orders      = cur_orders;
    s.total_trades      = cur_trades;
    s.total_rejections  = rejections_.load();
    s.total_liquidations = liquidations_.load();
    s.orders_per_sec    = (cur_orders - orders_at_last_snap_) / elapsed;
    s.trades_per_sec    = (cur_trades - trades_at_last_snap_) / elapsed;
    s.fill_rate_pct     = (cur_orders > 0)
        ? 100.0 * cur_trades / cur_orders : 0.0;

    orders_at_last_snap_ = cur_orders;
    trades_at_last_snap_ = cur_trades;
    last_snapshot_time_  = now;

    // Latency percentiles
    {
        std::lock_guard<std::mutex> lk(lat_mutex_);
        if (!lat_samples_.empty()) {
            std::vector<uint64_t> sorted = lat_samples_;
            std::sort(sorted.begin(), sorted.end());
            size_t n = sorted.size();
            s.lat_p50 = sorted[n * 50 / 100];
            s.lat_p95 = sorted[n * 95 / 100];
            s.lat_p99 = sorted[n * 99 / 100];
            s.lat_max = sorted.back();
        }
        lat_samples_.clear();
    }

    // Market / risk
    {
        std::lock_guard<std::mutex> lk(mkt_mutex_);
        s.last_price        = last_price_;
        s.best_bid          = best_bid_;
        s.best_ask          = best_ask_;
        s.spread            = best_ask_ - best_bid_;
        s.vault_balance     = vault_balance_;
        s.vault_utilization = vault_utilization_;
        s.zero_sum_error    = zero_sum_error_;
    }

    // Top winners / losers
    {
        std::lock_guard<std::mutex> lk(pnl_mutex_);
        std::vector<std::pair<std::string, double>> pnl_vec(
            cumulative_pnl_.begin(), cumulative_pnl_.end());
        std::sort(pnl_vec.begin(), pnl_vec.end(),
            [](const auto& a, const auto& b){ return a.second > b.second; });
        int top = std::min(10, (int)pnl_vec.size());
        s.top_winners.assign(pnl_vec.begin(), pnl_vec.begin() + top);
        std::sort(pnl_vec.begin(), pnl_vec.end(),
            [](const auto& a, const auto& b){ return a.second < b.second; });
        s.top_losers.assign(pnl_vec.begin(), pnl_vec.begin() + top);
    }

    // Rejection breakdown
    {
        std::lock_guard<std::mutex> lk(rej_mutex_);
        std::ostringstream oss;
        bool first = true;
        for (const auto& [reason, count] : rej_counts_) {
            if (!first) oss << ", ";
            oss << reason << ":" << count;
            first = false;
        }
        s.rejection_breakdown = oss.str();
    }

    return s;
}

// ---------------------------------------------------------------------------
// Snapshot::to_string
// ---------------------------------------------------------------------------

std::string Snapshot::to_string() const {
    std::ostringstream o;
    o << std::fixed << std::setprecision(2);
    o << "┌─────────────────────────────────────────────┐\n";
    o << "│           ASGARD FnO  ─  Stats              │\n";
    o << "├─────────────────────────────────────────────┤\n";
    o << "│ Throughput                                   │\n";
    o << "│   Orders/s : " << std::setw(10) << orders_per_sec
      << "   Trades/s : " << std::setw(10) << trades_per_sec << "  │\n";
    o << "│   Fill rate: " << std::setw(6) << fill_rate_pct << "%"
      << "   Rejections: " << std::setw(8) << total_rejections << "  │\n";
    o << "│ Latency (μs)                                 │\n";
    o << "│   p50:" << std::setw(8) << lat_p50
      << "  p95:" << std::setw(8) << lat_p95
      << "  p99:" << std::setw(8) << lat_p99
      << "  max:" << std::setw(8) << lat_max << "│\n";
    o << "│ Market                                       │\n";
    o << "│   Last: $" << std::setw(10) << last_price
      << "  Bid: $" << std::setw(10) << best_bid << "  │\n";
    o << "│   Ask: $" << std::setw(10) << best_ask
      << "  Spread: $" << std::setw(8) << spread << "  │\n";
    o << "│ Risk                                         │\n";
    o << "│   Vault: $" << std::setw(12) << vault_balance
      << "  Util: " << std::setw(5) << vault_utilization * 100 << "%  │\n";
    o << "│   Zero-sum err: $" << std::setw(10) << zero_sum_error << "              │\n";
    o << "│   Liquidations: " << std::setw(8) << total_liquidations << "               │\n";
    o << "│ Top Winners                                  │\n";
    for (int i = 0; i < std::min(3, (int)top_winners.size()); ++i) {
        o << "│   " << std::setw(20) << top_winners[i].first
          << "  $" << std::setw(10) << top_winners[i].second << "       │\n";
    }
    o << "│ Top Losers                                   │\n";
    for (int i = 0; i < std::min(3, (int)top_losers.size()); ++i) {
        o << "│   " << std::setw(20) << top_losers[i].first
          << "  $" << std::setw(10) << top_losers[i].second << "       │\n";
    }
    if (!rejection_breakdown.empty()) {
        o << "│ Rejections: " << rejection_breakdown << "\n";
    }
    o << "└─────────────────────────────────────────────┘\n";
    return o.str();
}

} // namespace asgard::stats
