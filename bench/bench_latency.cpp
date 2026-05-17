// ---------------------------------------------------------------------------
// bench/bench_latency.cpp — Asgard Matching Engine Benchmark
//
// Output format mirrors the reference benchmark table for direct comparison:
//   Operation | Book depth | Latency | Throughput | Verdict
//
// Scenarios:
//   1. Order insertion (no match)   — GTC bid rests against pre-seeded book
//   2. Single match                 — aggressor hits best level
//   3. Multi-level sweep            — IOC sweeps N price levels
//   4. Deep level match (FIFO)      — N orders queued at same price
//   5. Cancel order                 — O(1) cancel via stored iterator
//   6. Market order (empty book)    — IOC with no resting liquidity
//   7. Post-only insert             — rests without crossing
//   8. Mixed workload (1000 ops)    — realistic add/cancel/match mix
// ---------------------------------------------------------------------------

#include "../engine/models.hpp"
#include "../engine/orderbook.hpp"
#include "../engine/sequencer.hpp"
#include "../engine/matching.hpp"
#include "../engine/risk_checks.hpp"
#include "../engine/margin.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace asgard;
using Clock = std::chrono::high_resolution_clock;

// ---------------------------------------------------------------------------
// Setup helpers
// ---------------------------------------------------------------------------

static uint64_t elapsed_ns(Clock::time_point t0) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - t0).count());
}

static Instrument make_inst() {
    Instrument i;
    i.symbol            = "BTC-USDC-PERP";
    i.state             = InstrumentState::TRADING;
    i.min_lot           = 0.001;
    i.max_lot           = 500.0;
    i.lot_step          = 0.001;
    i.price_band_pct    = 0.05;
    i.impact_band_pct   = 0.03;   // 3% — keeps sweep fills within band
    i.base_imf          = 0.02;
    i.imf_factor        = 0.00003;
    i.base_mmf          = 0.01;
    i.mmf_factor        = 0.000015;
    i.max_position_size = 500.0;
    i.maker_fee_rate    = 0.0002;
    i.taker_fee_rate    = 0.0005;
    i.liq_fee_rate      = 0.01;
    i.liq_chunk_pct     = 0.10;
    i.liq_band_pct      = 0.05;
    return i;
}

static UserAccount make_user(const std::string& id, double bal = 10'000'000.0) {
    UserAccount u;
    u.user_id         = id;
    u.wallet_balance  = bal;
    u.total_deposited = bal;
    u.rate_limit      = 10'000'000;
    u.user_type       = UserType::ALGO;
    return u;
}

static Order make_order(const std::string& id, const std::string& uid,
                         OrderSide side, double price, double qty,
                         OrderType type = OrderType::LIMIT,
                         TIF tif = TIF::GTC) {
    Order o;
    o.order_id      = id;
    o.user_id       = uid;
    o.instrument    = "BTC-USDC-PERP";
    o.side          = side;
    o.type          = type;
    o.price         = price;
    o.quantity      = qty;
    o.remaining_qty = qty;
    o.tif           = tif;
    o.stp_mode      = STPMode::CANCEL_INCOMING;
    o.status        = OrderStatus::NEW;
    o.timestamp_us  = now_us();
    return o;
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

struct Stats {
    uint64_t p50, p95, p99;
    double   mean;
    double   tput;   // ops/sec based on p50
};

static Stats compute(std::vector<uint64_t>& s) {
    std::sort(s.begin(), s.end());
    size_t n = s.size();
    auto pct = [&](double p) -> uint64_t {
        size_t i = static_cast<size_t>(std::ceil(p * n / 100.0));
        return s[std::min(i, n - 1)];
    };
    double sum = 0;
    for (auto v : s) sum += v;
    uint64_t p50 = pct(50);
    return Stats{p50, pct(95), pct(99), sum / n,
                 p50 > 0 ? 1e9 / static_cast<double>(p50) : 0.0};
}

// ---------------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------------

static std::string fmt_ns(uint64_t v, double per_fill = 0.0) {
    std::ostringstream oss;
    if (v < 1000)
        oss << v << " ns";
    else
        oss << std::fixed << std::setprecision(1) << (v / 1000.0) << " µs";
    if (per_fill > 0.5) {
        uint64_t pf = static_cast<uint64_t>(per_fill);
        oss << " (~" << pf << " ns/fill)";
    }
    return oss.str();
}

static std::string fmt_tput(double t, const char* unit = "ops/sec") {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1);
    if (t >= 1e6)      oss << (t / 1e6) << "M " << unit;
    else if (t >= 1e3) oss << (t / 1e3) << "K " << unit;
    else               oss << static_cast<uint64_t>(t) << " " << unit;
    return oss.str();
}

static std::string verdict(uint64_t p50_per_op) {
    if (p50_per_op <= 150) return "Excellent";
    if (p50_per_op <= 300) return "Good";
    if (p50_per_op <= 1500) return "Acceptable";
    return "Needs improvement";
}

// ---------------------------------------------------------------------------
// Result row
// ---------------------------------------------------------------------------

struct Row {
    std::string op, depth, latency, throughput, verd;
    uint64_t    p50_raw;   // raw p50 for verdict coloring
};

// ---------------------------------------------------------------------------
// 1. Order insertion (no match)
//
// Book pre-seeded with `depth` ask levels.  Each insertion is a GTC bid
// that rests on the bid side without crossing.  Bids cycle through `depth`
// price levels so the bid side also reaches `depth` levels after warm-up.
// ---------------------------------------------------------------------------

static Row bench_insert(int depth, int N) {
    const Instrument inst = make_inst();
    std::unordered_map<std::string, UserAccount> accts;
    accts["mk"] = make_user("mk");
    accts["tk"] = make_user("tk");

    OrderBook      book;
    Sequencer      seq;
    MatchingEngine eng(book, seq, inst, accts);

    // Seed ask side
    for (int d = 0; d < depth; ++d) {
        auto o = make_order("A" + std::to_string(d), "mk",
                             OrderSide::SELL, 51000.0 + d * 0.5, 1.0);
        eng.process(o, 50000.0);
    }

    std::vector<uint64_t> s;
    s.reserve(N);

    for (int i = 0; i < N; ++i) {
        // Cycle through `depth` bid prices — bid side builds to `depth` levels
        double px = 49000.0 - (i % depth) * 0.5;
        auto o = make_order("B" + std::to_string(i), "tk",
                             OrderSide::BUY, px, 1.0);
        auto t0 = Clock::now();
        eng.process(o, 50000.0);
        s.push_back(elapsed_ns(t0));
    }

    auto st = compute(s);
    std::string ds = std::to_string(depth) + " levels";
    return Row{"Order insertion (no match)", ds,
               fmt_ns(st.p50), fmt_tput(st.tput),
               verdict(st.p50), st.p50};
}

// ---------------------------------------------------------------------------
// 2. Single match
//
// Book has `depth` levels.  Each iteration: refill best ask at 50000,
// then measure an aggressor buy that fills it exactly.
// ---------------------------------------------------------------------------

static Row bench_single_match(int depth, int N) {
    const Instrument inst = make_inst();
    std::unordered_map<std::string, UserAccount> accts;
    accts["mk"] = make_user("mk");
    accts["tk"] = make_user("tk");

    OrderBook      book;
    Sequencer      seq;
    MatchingEngine eng(book, seq, inst, accts);

    // Seed depth-1 asks above 50000 (persist throughout)
    for (int d = 1; d < depth; ++d) {
        auto o = make_order("A" + std::to_string(d), "mk",
                             OrderSide::SELL, 50000.0 + d, 1.0);
        eng.process(o, 50000.0);
    }

    std::vector<uint64_t> s;
    s.reserve(N);

    for (int i = 0; i < N; ++i) {
        // Refill best ask (consumed in previous iteration)
        auto sell = make_order("RS" + std::to_string(i), "mk",
                                OrderSide::SELL, 50000.0, 1.0);
        eng.process(sell, 50000.0);

        // Measure: aggressor buy fills the best ask
        auto buy = make_order("RB" + std::to_string(i), "tk",
                               OrderSide::BUY, 50000.0, 1.0);
        auto t0 = Clock::now();
        eng.process(buy, 50000.0);
        s.push_back(elapsed_ns(t0));
    }

    auto st = compute(s);
    std::string ds = std::to_string(depth) + " levels";
    return Row{"Single match", ds,
               fmt_ns(st.p50), fmt_tput(st.tput),
               verdict(st.p50), st.p50};
}

// ---------------------------------------------------------------------------
// 3. Multi-level sweep
//
// IOC buy sweeps `levels` distinct price levels in one call.
// Reports total latency + per-fill ns.
// ---------------------------------------------------------------------------

static Row bench_sweep(int levels, int N) {
    const Instrument inst = make_inst();
    std::unordered_map<std::string, UserAccount> accts;
    accts["mk"] = make_user("mk");
    accts["tk"] = make_user("tk", 100'000'000.0);

    std::vector<uint64_t> s;
    s.reserve(N);

    for (int iter = 0; iter < N; ++iter) {
        OrderBook      book;
        Sequencer      seq;
        MatchingEngine eng(book, seq, inst, accts);
        // Reset account state between iterations
        accts["mk"].open_orders.clear();
        accts["mk"].open_order_margin = 0.0;
        accts["tk"].open_orders.clear();
        accts["tk"].open_order_margin = 0.0;

        for (int l = 0; l < levels; ++l) {
            auto sell = make_order("SL" + std::to_string(iter * levels + l),
                                    "mk", OrderSide::SELL, 50000.0 + l, 1.0);
            eng.process(sell, 50000.0);
        }

        auto buy = make_order("SW" + std::to_string(iter), "tk",
                               OrderSide::BUY, 50000.0 + levels,
                               static_cast<double>(levels),
                               OrderType::LIMIT, TIF::IOC);
        auto t0 = Clock::now();
        eng.process(buy, 50000.0);
        s.push_back(elapsed_ns(t0));
    }

    auto st = compute(s);
    double per_fill = (levels > 0) ? static_cast<double>(st.p50) / levels : 0.0;
    double fills_sec = (st.p50 > 0) ? (levels * 1e9 / st.p50) : 0.0;
    std::string ds = std::to_string(levels) + (levels == 1 ? " level" : " levels");
    uint64_t pf_u = static_cast<uint64_t>(per_fill);
    return Row{"Multi-level sweep", ds,
               fmt_ns(st.p50, levels > 1 ? per_fill : 0.0),
               fmt_tput(fills_sec, "fills/sec"),
               verdict(pf_u > 0 ? pf_u : st.p50), st.p50};
}

// ---------------------------------------------------------------------------
// 4. Deep level match (FIFO)
//
// `k` orders queued at the same price level.  One aggressor IOC fills all k.
// Reports total latency + per-fill ns.
// ---------------------------------------------------------------------------

static Row bench_deep_fifo(int k, int N) {
    const Instrument inst = make_inst();
    std::unordered_map<std::string, UserAccount> accts;
    accts["mk"] = make_user("mk");
    accts["tk"] = make_user("tk", 100'000'000.0);

    std::vector<uint64_t> s;
    s.reserve(N);

    for (int iter = 0; iter < N; ++iter) {
        OrderBook      book;
        Sequencer      seq;
        MatchingEngine eng(book, seq, inst, accts);
        accts["mk"].open_orders.clear();
        accts["mk"].open_order_margin = 0.0;
        accts["tk"].open_orders.clear();
        accts["tk"].open_order_margin = 0.0;

        for (int j = 0; j < k; ++j) {
            auto sell = make_order("DF" + std::to_string(iter * k + j),
                                    "mk", OrderSide::SELL, 50000.0, 1.0);
            eng.process(sell, 50000.0);
        }

        auto buy = make_order("DFB" + std::to_string(iter), "tk",
                               OrderSide::BUY, 50000.0,
                               static_cast<double>(k),
                               OrderType::LIMIT, TIF::IOC);
        auto t0 = Clock::now();
        eng.process(buy, 50000.0);
        s.push_back(elapsed_ns(t0));
    }

    auto st = compute(s);
    double per_fill = (k > 0) ? static_cast<double>(st.p50) / k : 0.0;
    double fills_sec = (st.p50 > 0) ? (k * 1e9 / st.p50) : 0.0;
    std::string ds = std::to_string(k) + (k == 1 ? " order" : " orders");
    uint64_t pf_u = static_cast<uint64_t>(per_fill);
    return Row{"Deep level match (FIFO)", ds,
               fmt_ns(st.p50, k > 1 ? per_fill : 0.0),
               fmt_tput(fills_sec, "fills/sec"),
               verdict(pf_u > 0 ? pf_u : st.p50), st.p50};
}

// ---------------------------------------------------------------------------
// 5. Cancel order
//
// Book has `depth` price levels with N orders spread evenly across them.
// O(1) cancel via stored iterator — latency should be flat vs depth.
// ---------------------------------------------------------------------------

static Row bench_cancel(int depth, int N) {
    const Instrument inst = make_inst();
    std::unordered_map<std::string, UserAccount> accts;
    accts["mk"] = make_user("mk");

    OrderBook      book;
    Sequencer      seq;
    MatchingEngine eng(book, seq, inst, accts);

    // N orders spread across `depth` levels (may be multiple orders/level)
    std::vector<std::string> oids;
    oids.reserve(N);
    for (int i = 0; i < N; ++i) {
        int    lv = i % depth;
        double px = 50000.0 + lv * 1.0;
        std::string oid = "CO" + std::to_string(i);
        auto o = make_order(oid, "mk", OrderSide::SELL, px, 1.0);
        eng.process(o, 50000.0);
        oids.push_back(oid);
    }

    // Shuffle to avoid sequential-access bias
    std::mt19937 rng(42);
    std::shuffle(oids.begin(), oids.end(), rng);

    std::vector<uint64_t> s;
    s.reserve(N);

    for (auto& oid : oids) {
        auto t0 = Clock::now();
        eng.cancel(oid, accts["mk"], inst.symbol);
        s.push_back(elapsed_ns(t0));
    }

    auto st = compute(s);
    std::string ds;
    if      (depth >= 10000) ds = "10,000 levels";
    else if (depth >= 1000)  ds = "1,000 levels";
    else                     ds = std::to_string(depth) + " levels";

    return Row{"Cancel order", ds,
               fmt_ns(st.p50), fmt_tput(st.tput),
               verdict(st.p50), st.p50};
}

// ---------------------------------------------------------------------------
// 6. Market order (empty book)
//
// IOC market buy with no resting asks — cancelled immediately.
// Tests the overhead of the engine pipeline with zero matching work.
// ---------------------------------------------------------------------------

static Row bench_market_empty(int N) {
    const Instrument inst = make_inst();
    std::unordered_map<std::string, UserAccount> accts;
    accts["tk"] = make_user("tk");

    OrderBook      book;
    Sequencer      seq;
    MatchingEngine eng(book, seq, inst, accts);

    std::vector<uint64_t> s;
    s.reserve(N);

    for (int i = 0; i < N; ++i) {
        auto o = make_order("MK" + std::to_string(i), "tk",
                             OrderSide::BUY, 0.0, 1.0,
                             OrderType::MARKET, TIF::IOC);
        auto t0 = Clock::now();
        eng.process(o, 50000.0);
        s.push_back(elapsed_ns(t0));
    }

    auto st = compute(s);
    return Row{"Market order (empty book)", "0",
               fmt_ns(st.p50), fmt_tput(st.tput),
               verdict(st.p50), st.p50};
}

// ---------------------------------------------------------------------------
// 7. Post-only insert
//
// Book has 100 ask levels.  POST_ONLY bid priced well below best ask — rests.
// ---------------------------------------------------------------------------

static Row bench_post_only(int N) {
    const Instrument inst = make_inst();
    std::unordered_map<std::string, UserAccount> accts;
    accts["mk"] = make_user("mk");
    accts["tk"] = make_user("tk");

    OrderBook      book;
    Sequencer      seq;
    MatchingEngine eng(book, seq, inst, accts);

    for (int d = 0; d < 100; ++d) {
        auto o = make_order("PA" + std::to_string(d), "mk",
                             OrderSide::SELL, 51000.0 + d, 1.0);
        eng.process(o, 50000.0);
    }

    std::vector<uint64_t> s;
    s.reserve(N);

    for (int i = 0; i < N; ++i) {
        // Cycle through bid levels so bids don't pile up at one price
        double px = 49000.0 - (i % 100) * 0.5;
        auto o = make_order("PO" + std::to_string(i), "tk",
                             OrderSide::BUY, px, 1.0);
        o.type = OrderType::POST_ONLY;
        auto t0 = Clock::now();
        eng.process(o, 50000.0);
        s.push_back(elapsed_ns(t0));
    }

    auto st = compute(s);
    return Row{"Post-only insert", "100 levels",
               fmt_ns(st.p50), fmt_tput(st.tput),
               verdict(st.p50), st.p50};
}

// ---------------------------------------------------------------------------
// 8. Mixed workload (1000 ops/iteration)
//
// 40% add GTC bid, 30% add GTC ask, 10% cancel, 20% market IOC.
// Reports total latency for 1000 ops (p50 across `iters` iterations).
// ---------------------------------------------------------------------------

static Row bench_mixed(int iters) {
    const Instrument inst = make_inst();
    std::unordered_map<std::string, UserAccount> accts;
    accts["mk"] = make_user("mk", 100'000'000.0);
    accts["tk"] = make_user("tk", 100'000'000.0);

    std::mt19937 rng(123);
    std::vector<uint64_t> total_times;
    total_times.reserve(iters);

    for (int iter = 0; iter < iters; ++iter) {
        OrderBook      book;
        Sequencer      seq;
        MatchingEngine eng(book, seq, inst, accts);
        accts["mk"].open_orders.clear();
        accts["mk"].open_order_margin = 0.0;
        accts["tk"].open_orders.clear();
        accts["tk"].open_order_margin = 0.0;

        std::vector<std::string> live_bids, live_asks;
        live_bids.reserve(500);
        live_asks.reserve(300);

        auto t_start = Clock::now();

        for (int op = 0; op < 1000; ++op) {
            std::string oid = "MX" + std::to_string(op);
            int r = static_cast<int>(rng() % 10);

            if (r < 4) {
                // 40%: GTC bid (well below any ask — rests)
                double px = 47000.0 + (rng() % 2000) * 0.5;
                auto o = make_order(oid, "mk", OrderSide::BUY, px, 1.0);
                eng.process(o, 50000.0);
                if (accts["mk"].open_orders.count(oid))
                    live_bids.push_back(oid);

            } else if (r < 7) {
                // 30%: GTC ask (well above any bid — rests)
                double px = 53000.0 + (rng() % 2000) * 0.5;
                auto o = make_order(oid, "mk", OrderSide::SELL, px, 1.0);
                eng.process(o, 50000.0);
                if (accts["mk"].open_orders.count(oid))
                    live_asks.push_back(oid);

            } else if (r < 8 && !live_bids.empty()) {
                // 10%: cancel a resting bid
                const std::string& cid = live_bids.back();
                eng.cancel(cid, accts["mk"], inst.symbol);
                live_bids.pop_back();

            } else {
                // 20%: IOC market order (may or may not fill)
                OrderSide side = (rng() % 2 == 0) ? OrderSide::BUY
                                                   : OrderSide::SELL;
                auto o = make_order(oid, "tk", side, 0.0, 1.0,
                                     OrderType::MARKET, TIF::IOC);
                eng.process(o, 50000.0);
            }
        }

        total_times.push_back(elapsed_ns(t_start));
    }

    std::sort(total_times.begin(), total_times.end());
    uint64_t p50_total = total_times[total_times.size() / 2];
    double per_op_ns = static_cast<double>(p50_total) / 1000.0;
    double tput = (per_op_ns > 0.0) ? (1e9 / per_op_ns) : 0.0;

    std::string lat_str;
    {
        std::ostringstream oss;
        if (p50_total < 1000)
            oss << p50_total << " ns total";
        else
            oss << std::fixed << std::setprecision(1)
                << (p50_total / 1000.0) << " µs total";
        lat_str = oss.str();
    }

    return Row{"Mixed workload (1000 ops)", "Growing",
               lat_str, fmt_tput(tput),
               verdict(static_cast<uint64_t>(per_op_ns)), p50_total};
}

// ---------------------------------------------------------------------------
// Print table (matches reference visual format)
// ---------------------------------------------------------------------------

static void print_table(const std::vector<Row>& rows) {
    const int W_OP    = 30;
    const int W_DEPTH = 14;
    const int W_LAT   = 26;
    const int W_TPUT  = 18;
    const int W_VRD   = 22;

    auto hline = [&]() {
        std::cout << '+'
                  << std::string(W_OP + 2, '-') << '+'
                  << std::string(W_DEPTH + 2, '-') << '+'
                  << std::string(W_LAT + 2, '-') << '+'
                  << std::string(W_TPUT + 2, '-') << '+'
                  << std::string(W_VRD + 2, '-') << "+\n";
    };

    auto cell = [](const std::string& s, int w) -> std::string {
        if (static_cast<int>(s.size()) >= w) return s.substr(0, w);
        return s + std::string(w - s.size(), ' ');
    };

    auto row = [&](const std::string& op,    const std::string& depth,
                    const std::string& lat,   const std::string& tput,
                    const std::string& vrd) {
        std::cout << "| " << cell(op, W_OP)
                  << " | " << cell(depth, W_DEPTH)
                  << " | " << cell(lat, W_LAT)
                  << " | " << cell(tput, W_TPUT)
                  << " | " << cell(vrd, W_VRD)
                  << " |\n";
    };

    hline();
    row("Operation", "Book depth", "Latency (p50)", "Throughput", "Verdict");
    hline();

    std::string last_op;
    for (auto& r : rows) {
        std::string op_disp = (r.op == last_op) ? "" : r.op;
        last_op = r.op;
        row(op_disp, r.depth, r.latency, r.throughput, r.verd);
    }

    hline();
}

// ---------------------------------------------------------------------------
// Warmup — prime caches, JIT-style stabilisation
// ---------------------------------------------------------------------------

static void warmup() {
    Instrument inst = make_inst();
    std::unordered_map<std::string, UserAccount> accts;
    accts["w"] = make_user("w");
    OrderBook book; Sequencer seq;
    MatchingEngine eng(book, seq, inst, accts);
    for (int i = 0; i < 10'000; ++i) {
        auto s = make_order("WS" + std::to_string(i), "w",
                             OrderSide::SELL, 50000.0, 1.0);
        eng.process(s, 50000.0);
        auto b = make_order("WB" + std::to_string(i), "w",
                             OrderSide::BUY,  50000.0, 1.0,
                             OrderType::LIMIT, TIF::IOC);
        eng.process(b, 50000.0);
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    std::cout << "+----------------------------------------------------------------------+\n";
    std::cout << "|         Asgard Matching Engine — Benchmark Results                   |\n";
    std::cout << "+----------------------------------------------------------------------+\n\n";
    std::cout << "Warming up...\n";
    warmup();
    std::cout << "Running benchmarks...\n\n";

    std::vector<Row> results;

    // 1. Order insertion (no match)
    for (int d : {10, 100, 1000, 10000})
        results.push_back(bench_insert(d, 50'000));

    // 2. Single match
    for (int d : {10, 100, 1000})
        results.push_back(bench_single_match(d, 30'000));

    // 3. Multi-level sweep
    for (int l : {1, 5, 10, 50, 100})
        results.push_back(bench_sweep(l, 5'000));

    // 4. Deep level match (FIFO)
    for (int k : {1, 10, 50, 100})
        results.push_back(bench_deep_fifo(k, 5'000));

    // 5. Cancel order (O(1) via stored iterator — flat vs depth)
    for (int d : {10, 100, 1000, 10000})
        results.push_back(bench_cancel(d, 10'000));

    // 6. Market order (empty book)
    results.push_back(bench_market_empty(50'000));

    // 7. Post-only insert
    results.push_back(bench_post_only(50'000));

    // 8. Mixed workload
    results.push_back(bench_mixed(300));

    print_table(results);

    std::cout << "\nNote: Cancel latency is O(1) via stored iterator regardless of book depth.\n";
    std::cout << "      Reference engine shows O(n) cancel degradation at 1K-10K levels.\n";

    return 0;
}
