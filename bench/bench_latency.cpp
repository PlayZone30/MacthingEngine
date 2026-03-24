// ---------------------------------------------------------------------------
// bench/bench_latency.cpp — Asgard Matching Engine Latency Benchmark
//
// Measures nanosecond-level latency for:
//   Scenario A: add_order()           — resting a GTC limit on an empty book
//   Scenario B: immediate fill        — single aggressor hits single resting
//   Scenario C: partial fill          — aggressor partially fills, rests residual
//   Scenario D: sweep N levels        — aggressor walks through 10 price levels
//   Scenario E: full pipeline         — risk_checks + match (no actual fills)
//   Scenario F: cancel resting order  — remove_order() hot path
//
// Reports: p50, p90, p95, p99, p99.9, max, mean  (all in nanoseconds)
// ---------------------------------------------------------------------------

#include "../engine/models.hpp"
#include "../engine/orderbook.hpp"
#include "../engine/sequencer.hpp"
#include "../engine/matching.hpp"
#include "../engine/risk_checks.hpp"
#include "../engine/margin.hpp"

#include <algorithm>
#include <chrono>
#include <random>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>

using namespace asgard;
using Clock = std::chrono::high_resolution_clock;
using ns    = std::chrono::nanoseconds;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static uint64_t elapsed_ns(Clock::time_point start) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<ns>(Clock::now() - start).count());
}

static Instrument make_inst() {
    Instrument i;
    i.symbol            = "BTC-USDC-PERP";
    i.state             = InstrumentState::TRADING;
    i.min_lot           = 0.001;
    i.max_lot           = 500.0;
    i.lot_step          = 0.001;
    i.price_band_pct    = 0.05;
    i.impact_band_pct   = 0.02;
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

static UserAccount make_user(const std::string& id, double balance = 1'000'000.0) {
    UserAccount u;
    u.user_id         = id;
    u.wallet_balance  = balance;
    u.total_deposited = balance;
    u.rate_limit      = 10000;
    u.user_type       = UserType::ALGO;
    return u;
}

static Order make_limit(const std::string& id, const std::string& user,
                         OrderSide side, double price, double qty,
                         TIF tif = TIF::GTC) {
    Order o;
    o.order_id      = id;
    o.user_id       = user;
    o.instrument    = "BTC-USDC-PERP";
    o.side          = side;
    o.type          = OrderType::LIMIT;
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
// Stats
// ---------------------------------------------------------------------------

struct Stats {
    std::string name;
    uint64_t    n;
    double      mean_ns;
    uint64_t    p50, p90, p95, p99, p999, max_ns;
    double      throughput; // ops/sec

    std::string to_string() const {
        auto bar = [](uint64_t ns_val) -> std::string {
            // Each '#' = 100ns
            int blocks = static_cast<int>(ns_val / 100);
            if (blocks > 60) blocks = 60;
            return std::string(blocks, '#');
        };

        std::ostringstream oss;
        oss << "\n┌─ " << name << " (n=" << n << ", tput="
            << std::fixed << std::setprecision(0) << throughput << " ops/s)\n";
        oss << "│  mean  " << std::setw(8) << static_cast<uint64_t>(mean_ns) << " ns\n";
        oss << "│  p50   " << std::setw(8) << p50   << " ns  " << bar(p50)   << "\n";
        oss << "│  p90   " << std::setw(8) << p90   << " ns  " << bar(p90)   << "\n";
        oss << "│  p95   " << std::setw(8) << p95   << " ns  " << bar(p95)   << "\n";
        oss << "│  p99   " << std::setw(8) << p99   << " ns  " << bar(p99)   << "\n";
        oss << "│  p99.9 " << std::setw(8) << p999  << " ns  " << bar(p999)  << "\n";
        oss << "│  max   " << std::setw(8) << max_ns << " ns  " << bar(max_ns) << "\n";
        oss << "└──────────────────────────────────────────────\n";
        return oss.str();
    }
};

static Stats compute_stats(const std::string& name, std::vector<uint64_t>& samples) {
    std::sort(samples.begin(), samples.end());
    size_t n = samples.size();

    auto pct = [&](double p) -> uint64_t {
        size_t idx = static_cast<size_t>(std::ceil(p * n / 100.0));
        if (idx >= n) idx = n - 1;
        return samples[idx];
    };

    double sum = std::accumulate(samples.begin(), samples.end(), 0.0);

    // Throughput: total time = sum of individual latencies is NOT right —
    // they were sequential, so wall time ≈ sum.  Use sum to get ops/sec.
    double wall_ns = static_cast<double>(sum);
    double tput    = (wall_ns > 0.0) ? (n * 1e9 / wall_ns) : 0.0;

    return Stats{
        name, n,
        sum / n,
        pct(50), pct(90), pct(95), pct(99), pct(99.9),
        samples.back(),
        tput
    };
}

// ---------------------------------------------------------------------------
// Scenario A: add_order (resting)
// ---------------------------------------------------------------------------

static Stats bench_add_order(int N) {
    OrderBook book;
    Instrument inst = make_inst();

    // Pre-generate orders (exclude generation time)
    std::vector<Order> orders;
    orders.reserve(N);
    for (int i = 0; i < N; ++i) {
        double price = 50000.0 - (i % 500) * 0.1; // spread across 500 levels
        orders.push_back(make_limit("B" + std::to_string(i), "U1",
                                    OrderSide::BUY, price, 1.0));
    }

    std::vector<uint64_t> samples;
    samples.reserve(N);

    for (auto& o : orders) {
        auto t0 = Clock::now();
        book.add_order(o);
        samples.push_back(elapsed_ns(t0));
    }

    return compute_stats("A: add_order (resting GTC)", samples);
}

// ---------------------------------------------------------------------------
// Scenario B: immediate single fill
// ---------------------------------------------------------------------------

static Stats bench_immediate_fill(int N) {
    Instrument inst = make_inst();
    std::unordered_map<std::string, UserAccount> accounts;
    accounts["maker"] = make_user("maker");
    accounts["taker"] = make_user("taker");

    std::vector<uint64_t> samples;
    samples.reserve(N);

    // Each iteration: fresh book + 1 resting sell + 1 aggressor buy
    // (book reset per iteration to keep state clean)
    for (int i = 0; i < N; ++i) {
        OrderBook book;
        Sequencer seq;
        MatchingEngine eng(book, seq, inst, accounts);

        // Rest a sell (not measured)
        auto sell = make_limit("S" + std::to_string(i), "maker",
                               OrderSide::SELL, 50000.0, 1.0);
        eng.process(sell, 50000.0);

        // Measure aggressor buy
        auto buy = make_limit("B" + std::to_string(i), "taker",
                              OrderSide::BUY, 50000.0, 1.0);
        auto t0 = Clock::now();
        auto trades = eng.process(buy, 50000.0);
        samples.push_back(elapsed_ns(t0));

        (void)trades;
    }

    return compute_stats("B: immediate fill (1 resting → 1 trade)", samples);
}

// ---------------------------------------------------------------------------
// Scenario C: partial fill — aggressor partially fills, residual rests
// ---------------------------------------------------------------------------

static Stats bench_partial_fill(int N) {
    Instrument inst = make_inst();
    std::unordered_map<std::string, UserAccount> accounts;
    accounts["maker"] = make_user("maker");
    accounts["taker"] = make_user("taker");

    std::vector<uint64_t> samples;
    samples.reserve(N);

    for (int i = 0; i < N; ++i) {
        OrderBook book;
        Sequencer seq;
        MatchingEngine eng(book, seq, inst, accounts);

        // 0.5 BTC resting sell
        auto sell = make_limit("S" + std::to_string(i), "maker",
                               OrderSide::SELL, 50000.0, 0.5);
        eng.process(sell, 50000.0);

        // 1.0 BTC aggressor buy → 0.5 fills, 0.5 rests as GTC
        auto buy = make_limit("B" + std::to_string(i), "taker",
                              OrderSide::BUY, 50000.0, 1.0);
        auto t0 = Clock::now();
        auto trades = eng.process(buy, 50000.0);
        samples.push_back(elapsed_ns(t0));

        (void)trades;
    }

    return compute_stats("C: partial fill (0.5 fill + 0.5 rests GTC)", samples);
}

// ---------------------------------------------------------------------------
// Scenario D: sweep N price levels
// ---------------------------------------------------------------------------

static Stats bench_sweep(int LEVELS, int ITERS) {
    Instrument inst = make_inst();
    std::unordered_map<std::string, UserAccount> accounts;
    accounts["maker"] = make_user("maker");
    accounts["taker"] = make_user("taker", 10'000'000.0);

    std::vector<uint64_t> samples;
    samples.reserve(ITERS);

    for (int iter = 0; iter < ITERS; ++iter) {
        OrderBook book;
        Sequencer seq;
        MatchingEngine eng(book, seq, inst, accounts);

        // Populate LEVELS sell levels
        for (int lvl = 0; lvl < LEVELS; ++lvl) {
            double price = 50000.0 + lvl * 1.0;
            auto sell = make_limit("S" + std::to_string(iter * LEVELS + lvl),
                                   "maker", OrderSide::SELL, price, 1.0);
            eng.process(sell, 50000.0);
        }

        // One big IOC buy sweeps all levels
        auto buy = make_limit("B" + std::to_string(iter), "taker",
                              OrderSide::BUY, 50000.0 + LEVELS * 1.0,
                              static_cast<double>(LEVELS), TIF::IOC);
        auto t0 = Clock::now();
        auto trades = eng.process(buy, 50000.0);
        samples.push_back(elapsed_ns(t0));

        (void)trades;
    }

    return compute_stats("D: sweep " + std::to_string(LEVELS)
                         + " price levels (IOC)", samples);
}

// ---------------------------------------------------------------------------
// Scenario E: full pipeline — risk checks + match (no fills, order rests)
// ---------------------------------------------------------------------------

static Stats bench_full_pipeline(int N) {
    Instrument inst = make_inst();
    std::unordered_map<std::string, UserAccount> accounts;
    accounts["U1"] = make_user("U1");

    OrderBook  book;
    Sequencer  seq;
    RiskEngine risk;
    MatchingEngine eng(book, seq, inst, accounts);

    // Spread orders across different prices so they don't match each other
    std::vector<Order> orders;
    orders.reserve(N);
    for (int i = 0; i < N; ++i) {
        double price = 40000.0 + i * 0.1; // well below mark, won't match
        orders.push_back(make_limit("O" + std::to_string(i), "U1",
                                    OrderSide::BUY, price, 0.01));
    }

    double mark = 50000.0;
    std::vector<uint64_t> samples;
    samples.reserve(N);

    for (auto& o : orders) {
        auto t0 = Clock::now();
        RiskResult rr = risk.check_all(o, accounts.at("U1"), inst, mark);
        if (rr.pass) {
            o.seq = seq.next("ORDER", o.order_id);
            eng.process(o, mark);
        }
        samples.push_back(elapsed_ns(t0));
    }

    return compute_stats("E: full pipeline (risk + match, resting)", samples);
}

// ---------------------------------------------------------------------------
// Scenario F: cancel resting order (remove_order)
// ---------------------------------------------------------------------------

static Stats bench_cancel(int N) {
    OrderBook  book;
    Instrument inst = make_inst();

    // Insert N orders
    std::vector<Order> orders;
    orders.reserve(N);
    for (int i = 0; i < N; ++i) {
        double price = 50000.0 - (i % 200) * 0.5;
        auto o = make_limit("C" + std::to_string(i), "U1",
                            OrderSide::BUY, price, 1.0);
        book.add_order(o);
        orders.push_back(o);
    }

    // Shuffle to avoid sequential-access bias
    std::mt19937 rng(42);
    std::shuffle(orders.begin(), orders.end(), rng);

    std::vector<uint64_t> samples;
    samples.reserve(N);

    for (auto& o : orders) {
        auto t0 = Clock::now();
        book.remove_order(o.order_id, o.price, o.side);
        samples.push_back(elapsed_ns(t0));
    }

    return compute_stats("F: cancel resting order (remove_order)", samples);
}

// ---------------------------------------------------------------------------
// Scenario G: sustained throughput (warm book, continuous fills)
// ---------------------------------------------------------------------------

static Stats bench_throughput(int N) {
    Instrument inst = make_inst();
    std::unordered_map<std::string, UserAccount> accounts;
    accounts["maker"] = make_user("maker");
    accounts["taker"] = make_user("taker");

    OrderBook book;
    Sequencer seq;
    MatchingEngine eng(book, seq, inst, accounts);

    // Pre-seed book with 1000 sell levels so it never empties
    for (int i = 0; i < 1000; ++i) {
        double price = 50000.0 + i * 0.1;
        auto sell = make_limit("SEED" + std::to_string(i), "maker",
                               OrderSide::SELL, price, 1000.0); // huge qty
        eng.process(sell, 50000.0);
    }

    std::vector<uint64_t> samples;
    samples.reserve(N);

    for (int i = 0; i < N; ++i) {
        // Each buy hits the best ask immediately
        auto buy = make_limit("T" + std::to_string(i), "taker",
                              OrderSide::BUY, 51000.0, 0.001, TIF::IOC);
        auto t0 = Clock::now();
        auto trades = eng.process(buy, 50000.0);
        samples.push_back(elapsed_ns(t0));
        (void)trades;
    }

    return compute_stats("G: sustained throughput (warm book, IOC fills)", samples);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    constexpr int WARMUP  = 5'000;
    constexpr int SAMPLES = 100'000;
    constexpr int SWEEP_ITERS = 10'000;

    std::cout << "╔══════════════════════════════════════════════════════════╗\n";
    std::cout << "║        Asgard Matching Engine — Latency Benchmark        ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════╝\n";
    std::cout << "Samples: " << SAMPLES << "  |  Warmup: " << WARMUP << "\n";
    std::cout << "Bar scale: each '#' = 100 ns\n\n";

    // Warmup pass (discarded) — cold-start caches skew results
    {
        Instrument inst = make_inst();
        std::unordered_map<std::string, UserAccount> accounts;
        accounts["w"] = make_user("w");
        OrderBook book;
        Sequencer seq;
        MatchingEngine eng(book, seq, inst, accounts);
        for (int i = 0; i < WARMUP; ++i) {
            auto sell = make_limit("WS" + std::to_string(i), "w",
                                   OrderSide::SELL, 50000.0, 1.0);
            eng.process(sell, 50000.0);
            auto buy = make_limit("WB" + std::to_string(i), "w",
                                  OrderSide::BUY, 50000.0, 1.0, TIF::IOC);
            eng.process(buy, 50000.0);
        }
    }

    auto a = bench_add_order(SAMPLES);
    auto b = bench_immediate_fill(SAMPLES);
    auto c = bench_partial_fill(SAMPLES);
    auto d1 = bench_sweep(5, SWEEP_ITERS);
    auto d2 = bench_sweep(10, SWEEP_ITERS);
    auto d3 = bench_sweep(50, SWEEP_ITERS);
    auto e = bench_full_pipeline(SAMPLES);
    auto f = bench_cancel(SAMPLES);
    auto g = bench_throughput(SAMPLES);

    std::cout << a.to_string();
    std::cout << b.to_string();
    std::cout << c.to_string();
    std::cout << d1.to_string();
    std::cout << d2.to_string();
    std::cout << d3.to_string();
    std::cout << e.to_string();
    std::cout << f.to_string();
    std::cout << g.to_string();

    // Summary table
    std::cout << "\n┌────────────────────────────────────────────────────────────────────┐\n";
    std::cout << "│ SUMMARY (nanoseconds)                                              │\n";
    std::cout << "├──────────────────────────────────────────┬───────┬───────┬─────────┤\n";
    std::cout << "│ Scenario                                 │  p50  │  p99  │ tput/s  │\n";
    std::cout << "├──────────────────────────────────────────┼───────┼───────┼─────────┤\n";

    auto row = [](const Stats& s) {
        std::ostringstream oss;
        oss << "│ " << std::left << std::setw(40) << s.name.substr(0,40)
            << " │ " << std::right << std::setw(5) << s.p50
            << " │ " << std::setw(5) << s.p99
            << " │ " << std::setw(7) << std::fixed << std::setprecision(0) << s.throughput
            << " │\n";
        return oss.str();
    };

    std::cout << row(a) << row(b) << row(c)
              << row(d1) << row(d2) << row(d3)
              << row(e) << row(f) << row(g);
    std::cout << "└──────────────────────────────────────────┴───────┴───────┴─────────┘\n";

    return 0;
}
