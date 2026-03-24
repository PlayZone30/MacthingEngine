// ---------------------------------------------------------------------------
// main.cpp — Asgard FnO Matching Engine Orchestrator
//
// Architecture:
//   - Market thread:  GBM tick → mark price → stop triggers → risk monitor
//   - Engine thread:  drain order queue → pre-trade risk → match → positions
//   - WebSocket/REST: uWS event loop on main thread (after threads started)
//   - Stats thread:   print snapshot every 10 seconds
// ---------------------------------------------------------------------------

#include "engine/models.hpp"
#include "engine/orderbook.hpp"
#include "engine/sequencer.hpp"
#include "engine/margin.hpp"
#include "engine/mark_price.hpp"
#include "engine/risk_checks.hpp"
#include "engine/matching.hpp"
#include "engine/positions.hpp"
#include "engine/stop_orders.hpp"
#include "engine/risk_monitor.hpp"
#include "engine/liquidation.hpp"
#include "engine/circuit_breakers.hpp"
#include "engine/insurance_vault.hpp"
#include "simulation/market_price.hpp"
#include "simulation/user_simulator.hpp"
#include "api/subscriptions.hpp"
#include "api/broadcaster.hpp"
#include "api/websocket_server.hpp"
#include "stats/stats_collector.hpp"

#include "concurrentqueue.h"
#include <nlohmann/json.hpp>

#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>
#include <csignal>
#include <sstream>
#include <iomanip>

using namespace asgard;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Global shutdown flag
// ---------------------------------------------------------------------------

static std::atomic<bool> g_running{true};

static void signal_handler(int) {
    g_running.store(false, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Instrument factory
// ---------------------------------------------------------------------------

static Instrument make_btc_perp() {
    Instrument inst;
    inst.symbol               = "BTC-USDC-PERP";
    inst.state                = InstrumentState::TRADING;
    inst.min_lot              = 0.001;
    inst.max_lot              = 500.0;
    inst.lot_step             = 0.001;
    inst.price_band_pct       = 0.05;   // ±5% from mark
    inst.impact_band_pct      = 0.02;   // max 2% impact per fill
    inst.velocity_threshold   = 0.08;   // 8% in 60s → COOLDOWN
    inst.velocity_window_s    = 60;
    inst.cooldown_duration_s  = 30;
    inst.base_imf             = 0.02;   // 2% base IM
    inst.imf_factor           = 0.00003;
    inst.base_mmf             = 0.01;   // 1% base MM
    inst.mmf_factor           = 0.000015;
    inst.max_position_size    = 500.0;
    inst.maker_fee_rate       = 0.0002;
    inst.taker_fee_rate       = 0.0005;
    inst.liq_fee_rate         = 0.01;
    inst.liq_chunk_pct        = 0.10;
    inst.liq_band_pct         = 0.05;
    return inst;
}

// ---------------------------------------------------------------------------
// JSON helpers for broadcasting
// ---------------------------------------------------------------------------

static nlohmann::json trade_to_json(const Trade& t) {
    return {
        {"trade_id",   t.trade_id},
        {"price",      t.price},
        {"quantity",   t.quantity},
        {"buyer_id",   t.buyer_id},
        {"seller_id",  t.seller_id},
        {"timestamp",  t.timestamp_us}
    };
}

static nlohmann::json mark_price_json(double mark, double index, double basis) {
    return {
        {"mark_price",  mark},
        {"index_price", index},
        {"basis",       basis}
    };
}

static nlohmann::json account_to_json(const UserAccount& acc, double mark_price) {
    double upnl = 0.0;
    for (const auto& [sym, pos] : acc.positions) {
        double dir = (pos.side == PositionSide::LONG) ? 1.0 : -1.0;
        upnl += dir * (mark_price - pos.entry_price) * pos.size;
    }
    nlohmann::json positions = nlohmann::json::array();
    for (const auto& [sym, pos] : acc.positions) {
        double dir = (pos.side == PositionSide::LONG) ? 1.0 : -1.0;
        double pnl = dir * (mark_price - pos.entry_price) * pos.size;
        positions.push_back({
            {"instrument",  sym},
            {"side",        pos.side == PositionSide::LONG ? "LONG" : "SHORT"},
            {"size",        pos.size},
            {"entry_price", pos.entry_price},
            {"unrealized_pnl", pnl}
        });
    }
    return {
        {"user_id",         acc.user_id},
        {"wallet_balance",  acc.wallet_balance},
        {"open_order_margin", acc.open_order_margin},
        {"unrealized_pnl",  upnl},
        {"equity",          acc.wallet_balance + upnl},
        {"positions",       positions}
    };
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    int num_users = 10'000;
    int port      = 9001;
    if (argc > 1) num_users = std::stoi(argv[1]);
    if (argc > 2) port      = std::stoi(argv[2]);

    std::cout << "[ASGARD] Starting with " << num_users << " users on port " << port << "\n";

    // -----------------------------------------------------------------------
    // 1. Core engine components
    // -----------------------------------------------------------------------

    Instrument           inst  = make_btc_perp();
    InsuranceVault       vault;
    OrderBook            book;
    Sequencer            sequencer;
    MarkPriceEngine      mark_engine(0.1);  // 100ms ticks

    // Order queue: simulation threads write, engine thread reads
    moodycamel::ConcurrentQueue<Order> order_queue(500'000);

    // Market price simulator (GBM, σ=60% annual, dt=100ms)
    sim::GBMSimulator gbm_market;
    mark_engine.reset(gbm_market.price());

    // -----------------------------------------------------------------------
    // 2. User simulation
    // -----------------------------------------------------------------------

    sim::UserSimulator simulator(order_queue, gbm_market, inst.symbol, num_users);
    auto& accounts = simulator.accounts();
    double total_deposited = simulator.total_deposited();

    std::cout << "[ASGARD] Users: " << simulator.user_count()
              << "  Total deposited: $" << std::fixed << std::setprecision(0)
              << total_deposited << "\n";

    // -----------------------------------------------------------------------
    // 3. Engine layer
    // -----------------------------------------------------------------------

    RiskEngine      risk_engine;
    MatchingEngine  matching(book, sequencer, inst, accounts);
    StopTriggerQueue stop_queue;
    RiskMonitor     risk_monitor;
    LiquidationEngine liq_engine(matching, vault, accounts, inst);
    CircuitBreakerManager cb_manager;

    // -----------------------------------------------------------------------
    // 4. API layer
    // -----------------------------------------------------------------------

    api::SubscriptionManager subs;
    api::Broadcaster         broadcaster(subs);

    // -----------------------------------------------------------------------
    // 5. Stats
    // -----------------------------------------------------------------------

    stats::StatsCollector stats;

    // -----------------------------------------------------------------------
    // 6. Connect callbacks
    // -----------------------------------------------------------------------

    risk_monitor.set_liquidation_callback(
        [&](const std::string& user_id, const std::string& instrument) {
            auto it = accounts.find(user_id);
            if (it == accounts.end()) return;
            double mark = mark_engine.mark();
            int chunks = liq_engine.trigger(user_id, instrument, mark);
            if (chunks > 0) {
                stats.record_liquidation();
                broadcaster.publish_private(user_id, "account.warnings",
                    {{"type", "LIQUIDATION"}, {"instrument", instrument}});
            }
        });

    risk_monitor.set_warning_callback(
        [&](const std::string& user_id, double equity, double total_im) {
            broadcaster.publish_private(user_id, "account.warnings",
                {{"type",      "MARGIN_WARNING"},
                 {"equity",    equity},
                 {"total_im",  total_im}});
        });

    liq_engine.set_adl_callback(
        [&](const std::string& user_id, const std::string& instrument,
            double fill_qty, double fill_price) {
            broadcaster.publish("liquidation." + instrument, {
                {"type",        "ADL"},
                {"user_id",     user_id},
                {"fill_qty",    fill_qty},
                {"fill_price",  fill_price}
            });
        });

    cb_manager.set_emergency_callback(
        [&](const std::string& reason) {
            std::cerr << "[EMERGENCY] " << reason << "\n";
            broadcaster.publish("markPrice." + inst.symbol,
                {{"type", "EMERGENCY"}, {"reason", reason}});
        });

    // -----------------------------------------------------------------------
    // 7. Market thread: GBM tick → mark price → stops → risk monitor
    // -----------------------------------------------------------------------

    std::thread market_thread([&] {
        auto acct_ptrs = simulator.account_ptrs();
        double last_traded_price = gbm_market.price();

        while (g_running.load(std::memory_order_relaxed)) {
            double index_price = gbm_market.tick();
            double mid         = book.mid_price();
            double mark        = mark_engine.update(index_price, mid > 0.0 ? mid : index_price);

            // Stop trigger scan
            stop_queue.on_price_update(mark, last_traded_price, index_price, inst,
                [&](Order triggered) {
                    order_queue.enqueue(std::move(triggered));
                });

            // Risk monitor scan
            risk_monitor.scan(acct_ptrs, mark, inst);

            // Circuit breakers (Layer 3 + 4)
            int64_t now_s = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            cb_manager.update(inst, mark, vault, now_s);

            // Broadcast mark price
            broadcaster.publish("markPrice." + inst.symbol,
                mark_price_json(mark, index_price, mark_engine.basis()));

            // Stats update
            stats.set_market_data(last_traded_price,
                book.best_bid().value_or(0.0),
                book.best_ask().value_or(0.0));
            stats.set_vault(vault.current_balance, vault.utilization());

            std::this_thread::sleep_for(100ms);
        }
    });

    // -----------------------------------------------------------------------
    // 8. Engine thread: drain queue → risk → match → post-trade
    // -----------------------------------------------------------------------

    std::thread engine_thread([&] {
        Order o;
        uint64_t order_count = 0;
        double   fee_revenue = 0.0;
        uint64_t zero_sum_check_interval = 1000;

        // Rate-limit window reset: every second
        int64_t rate_window = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        while (g_running.load(std::memory_order_relaxed)) {
            // Reset rate-limit counters every second
            int64_t now_s = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            if (now_s > rate_window) {
                rate_window = now_s;
                for (auto& [id, acc] : accounts) {
                    acc.messages_this_sec   = 0;
                    acc.rate_window_start_s = now_s;
                }
            }

            // Drain up to 1024 orders per iteration to allow context switches
            static constexpr int BATCH = 1024;
            int dequeued = 0;
            while (dequeued < BATCH && order_queue.try_dequeue(o)) {
                ++dequeued;

                // Cancel sentinel (from WS cancel_order or liquidation)
                if (o.status == OrderStatus::CANCELLED && !o.order_id.empty()
                    && o.quantity == 0.0) {
                    auto it = accounts.find(o.user_id);
                    if (it != accounts.end()) {
                        matching.cancel(o.order_id, it->second, o.instrument);
                    }
                    continue;
                }

                // Find account
                auto acc_it = accounts.find(o.user_id);
                if (acc_it == accounts.end()) continue;
                UserAccount& user = acc_it->second;

                // Rate limit increment
                user.messages_this_sec++;

                // Assign seq#
                o.seq = sequencer.next("NEW_ORDER", o.order_id);

                // Pre-trade risk checks
                double mark = mark_engine.mark();
                RiskResult rr = risk_engine.check_all(o, user, inst, mark);
                if (!rr.pass) {
                    stats.record_rejection(rr.reason);
                    continue;
                }
                stats.record_order(o);
                ++order_count;

                // Matching
                auto trades = matching.process(o, mark);

                // Post-trade: positions + zero-sum accounting
                for (auto& t : trades) {
                    auto& buyer  = accounts.at(t.buyer_id);
                    auto& seller = accounts.at(t.seller_id);

                    double buyer_pnl  = apply_trade(buyer,  t, true,  inst, mark);
                    double seller_pnl = apply_trade(seller, t, false, inst, mark);

                    fee_revenue += t.price * t.quantity *
                        (inst.maker_fee_rate + inst.taker_fee_rate);
                    vault.credit_fee(t.price * t.quantity *
                        (inst.maker_fee_rate + inst.taker_fee_rate) * 0.5);

                    stats.record_trade(t);
                    if (buyer_pnl  != 0.0) stats.update_pnl(t.buyer_id,  buyer_pnl);
                    if (seller_pnl != 0.0) stats.update_pnl(t.seller_id, seller_pnl);

                    // Broadcast trade
                    broadcaster.publish("trade." + inst.symbol, trade_to_json(t));

                    // Broadcast private account updates
                    broadcaster.publish_private(t.buyer_id, "account.positions",
                        account_to_json(buyer, mark));
                    broadcaster.publish_private(t.seller_id, "account.positions",
                        account_to_json(seller, mark));
                }

                // Broadcast L2 update after any fill or resting change
                if (!trades.empty() || o.status == OrderStatus::OPEN) {
                    broadcaster.publish("depth." + inst.symbol,
                        nlohmann::json::parse(book.snapshot_json(20, sequencer.current())));
                }

                // Zero-sum check every N orders
                if (order_count % zero_sum_check_interval == 0) {
                    auto ptrs = simulator.account_ptrs();
                    double err = ::asgard::zero_sum_check(
                        ptrs, mark, fee_revenue, vault.current_balance, total_deposited);
                    stats.set_zero_sum_error(std::abs(err));
                    if (std::abs(err) > 1.0) {
                        std::cerr << "[WARN] Zero-sum error: $" << err
                                  << " after " << order_count << " orders\n";
                    }
                }
            }

            if (dequeued == 0) {
                // Tiny pause when queue is empty to avoid 100% CPU
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        }
    });

    // -----------------------------------------------------------------------
    // 9. Stats thread: print every 10 seconds
    // -----------------------------------------------------------------------

    std::thread stats_thread([&] {
        while (g_running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(10s);
            auto snap = stats.snapshot();
            std::cout << snap.to_string() << std::flush;
        }
    });

    // -----------------------------------------------------------------------
    // 10. Start simulation (user order threads)
    // -----------------------------------------------------------------------

    simulator.start();
    std::cout << "[ASGARD] Simulation started.\n";

    // -----------------------------------------------------------------------
    // 11. WebSocket + REST server (blocks on main thread)
    // -----------------------------------------------------------------------

    api::EngineInterface iface;

    iface.get_depth = [&](int depth) -> nlohmann::json {
        std::string json_str = book.snapshot_json(depth, sequencer.current());
        return nlohmann::json::parse(json_str);
    };

    iface.get_ticker = [&]() -> nlohmann::json {
        auto bb = book.best_bid();
        auto ba = book.best_ask();
        return {
            {"instrument",  inst.symbol},
            {"mark_price",  mark_engine.mark()},
            {"index_price", mark_engine.index()},
            {"best_bid",    bb.has_value() ? bb.value() : 0.0},
            {"best_ask",    ba.has_value() ? ba.value() : 0.0},
            {"spread",      (bb && ba) ? (ba.value() - bb.value()) : 0.0},
            {"orders",      (int64_t)book.order_count()},
            {"trades",      (int64_t)stats.total_trades()}
        };
    };

    iface.get_account = [&](const std::string& user_id) -> nlohmann::json {
        auto it = accounts.find(user_id);
        if (it == accounts.end()) return {{"error", "user not found"}};
        return account_to_json(it->second, mark_engine.mark());
    };

    api::WebSocketServer ws_server(order_queue, subs, broadcaster, std::move(iface), port);
    ws_server.run(); // blocks until SIGINT/SIGTERM

    // -----------------------------------------------------------------------
    // 12. Shutdown
    // -----------------------------------------------------------------------

    g_running.store(false);
    simulator.stop();

    if (market_thread.joinable()) market_thread.join();
    if (engine_thread.joinable()) engine_thread.join();
    if (stats_thread.joinable())  stats_thread.join();

    // Final stats
    auto final_snap = stats.snapshot();
    std::cout << "\n[ASGARD] Final stats:\n" << final_snap.to_string();
    std::cout << "[ASGARD] Shutdown complete.\n";

    return 0;
}
