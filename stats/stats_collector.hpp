#pragma once

#include "../engine/models.hpp"
#include <atomic>
#include <vector>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <numeric>
#include <chrono>

namespace asgard::stats {

// ---------------------------------------------------------------------------
// StatsCollector
//
// Lock-free counters for the hot path; a mutex-protected latency histogram
// that is read/printed every 10 seconds from the monitor thread.
//
// Latency is measured from order.timestamp_us to trade.timestamp_us.
// ---------------------------------------------------------------------------

struct Snapshot {
    uint64_t total_orders      = 0;
    uint64_t total_trades      = 0;
    uint64_t total_rejections  = 0;
    uint64_t total_liquidations = 0;

    double   orders_per_sec    = 0.0;
    double   trades_per_sec    = 0.0;
    double   fill_rate_pct     = 0.0;

    // Latency percentiles (microseconds)
    uint64_t lat_p50 = 0;
    uint64_t lat_p95 = 0;
    uint64_t lat_p99 = 0;
    uint64_t lat_max = 0;

    // Market
    double   last_price        = 0.0;
    double   best_bid          = 0.0;
    double   best_ask          = 0.0;
    double   spread            = 0.0;

    // Risk
    double   vault_balance     = 0.0;
    double   vault_utilization = 0.0;
    double   zero_sum_error    = 0.0;

    // Top PnL
    std::vector<std::pair<std::string, double>> top_winners; // (user_id, pnl)
    std::vector<std::pair<std::string, double>> top_losers;

    std::string rejection_breakdown; // comma-sep reason:count

    std::string to_string() const;
};

class StatsCollector {
public:
    void record_order(const Order& o);
    void record_trade(const Trade& t);
    void record_rejection(const std::string& reason);
    void record_liquidation();

    // Called from risk monitor / position engine
    void update_pnl(const std::string& user_id, double realized_pnl);
    void set_market_data(double last, double bid, double ask);
    void set_vault(double balance, double utilization);
    void set_zero_sum_error(double err);

    // Build a snapshot (called from monitor thread, every 10s)
    Snapshot snapshot();

    // Counters (atomic, safe to read from any thread)
    uint64_t total_orders()     const { return orders_.load(); }
    uint64_t total_trades()     const { return trades_.load(); }
    uint64_t total_rejections() const { return rejections_.load(); }

private:
    // Hot-path atomics
    std::atomic<uint64_t> orders_{0};
    std::atomic<uint64_t> trades_{0};
    std::atomic<uint64_t> rejections_{0};
    std::atomic<uint64_t> liquidations_{0};

    // Latency samples (us): protected by lat_mutex_
    std::mutex                lat_mutex_;
    std::vector<uint64_t>     lat_samples_; // cleared per snapshot interval

    // PnL tracking per user
    std::mutex                pnl_mutex_;
    std::unordered_map<std::string, double> cumulative_pnl_;

    // Market / risk (updated by monitor thread, read by snapshot)
    std::mutex   mkt_mutex_;
    double last_price_ = 0.0, best_bid_ = 0.0, best_ask_ = 0.0;
    double vault_balance_ = 0.0, vault_utilization_ = 0.0;
    double zero_sum_error_ = 0.0;

    // Rejection reason breakdown
    std::mutex                  rej_mutex_;
    std::unordered_map<std::string, uint64_t> rej_counts_;

    // Throughput tracking
    std::chrono::steady_clock::time_point last_snapshot_time_ = std::chrono::steady_clock::now();
    uint64_t orders_at_last_snap_  = 0;
    uint64_t trades_at_last_snap_  = 0;
};

} // namespace asgard::stats
