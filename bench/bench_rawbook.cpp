// ---------------------------------------------------------------------------
// bench/bench_rawbook.cpp — Asgard Raw Order Book Layer Benchmark
//
// Benchmarks ONLY the OrderBook data structure — no account management,
// no margin calculation, no sequencer, no WAL, no trade ID generation.
//
// This is the apples-to-apples comparison against reference engines that
// benchmark their raw book layer.  Our full pipeline (bench_latency.cpp)
// adds ~100–200 ns of production overhead on top of these numbers.
//
// Scenarios:
//   A: add_order()             — insert resting order at various depths
//   B: best_level_ptr() +      — single-fill hot path (two book ops)
//      consume_level_front()
//   C: remove_order()          — O(1) cancel via stored iterator
//   D: multi-level sweep       — consume N levels in one loop
//   E: deep FIFO fill          — consume N orders at same price level
//   F: available_qty()         — FOK depth pre-check
//   G: snapshot_json()         — L2 depth snapshot
// ---------------------------------------------------------------------------

#include "../engine/models.hpp"
#include "../engine/orderbook.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace asgard;
using Clock = std::chrono::high_resolution_clock;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static uint64_t elapsed_ns(Clock::time_point t0) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - t0).count());
}

// Build a minimal Order without going through MatchingEngine::process().
// No account side-effects.
static Order make_order(const std::string& id, OrderSide side,
                         double price, double qty) {
    Order o;
    o.order_id      = id;
    o.user_id       = "u";
    o.instrument    = "BTC-USDC-PERP";
    o.side          = side;
    o.type          = OrderType::LIMIT;
    o.price         = price;
    o.quantity      = qty;
    o.remaining_qty = qty;
    o.tif           = TIF::GTC;
    o.stp_mode      = STPMode::CANCEL_INCOMING;
    o.status        = OrderStatus::NEW;
    o.timestamp_us  = 0;   // no syscall — excluded from measurement
    return o;
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

struct Stats {
    uint64_t p50, p90, p95, p99;
    double   mean;
    double   tput;    // ops/sec based on p50
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
    return Stats{p50, pct(90), pct(95), pct(99),
                 sum / n,
                 p50 > 0 ? 1e9 / static_cast<double>(p50) : 0.0};
}

// ---------------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------------

static std::string fmt_ns(uint64_t v, double per_op = 0.0) {
    std::ostringstream oss;
    if (v < 1000) oss << v << " ns";
    else          oss << std::fixed << std::setprecision(1) << (v/1000.0) << " µs";
    if (per_op > 0.5) oss << " (~" << static_cast<uint64_t>(per_op) << " ns/op)";
    return oss.str();
}

static std::string fmt_tput(double t, const char* unit = "ops/sec") {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1);
    if (t >= 1e6)      oss << (t/1e6) << "M " << unit;
    else if (t >= 1e3) oss << (t/1e3) << "K " << unit;
    else               oss << static_cast<uint64_t>(t) << " " << unit;
    return oss.str();
}

static const char* verdict(uint64_t p50) {
    if (p50 <= 80)  return "Excellent";
    if (p50 <= 150) return "Very Good";
    if (p50 <= 300) return "Good";
    if (p50 <= 800) return "Acceptable";
    return "Needs improvement";
}

// ---------------------------------------------------------------------------
// Result row
// ---------------------------------------------------------------------------

struct Row {
    std::string op, depth, latency, throughput, verd;
};

// ---------------------------------------------------------------------------
// A: add_order — insert resting limit
//
// Book pre-seeded with `depth` ask levels on the opposite side.
// Bids are inserted cycling through `depth` price slots so the bid side
// also reaches `depth` levels — measuring the steady-state insertion cost.
// ---------------------------------------------------------------------------

static Row bench_add(int depth, int N) {
    OrderBook book;

    // Seed ask side (not measured)
    for (int d = 0; d < depth; ++d) {
        auto o = make_order("SA" + std::to_string(d), OrderSide::SELL,
                             51000.0 + d * 0.5, 1.0);
        book.add_order(o);
    }

    std::vector<uint64_t> s;
    s.reserve(N);

    for (int i = 0; i < N; ++i) {
        // Cycle through `depth` bid prices — bid side builds to `depth` levels
        double px = 49000.0 - (i % depth) * 0.5;
        auto o = make_order("B" + std::to_string(i), OrderSide::BUY, px, 1.0);
        auto t0 = Clock::now();
        book.add_order(o);
        s.push_back(elapsed_ns(t0));
    }

    auto st = compute(s);
    return Row{"A: add_order (resting bid)", std::to_string(depth) + " levels",
               fmt_ns(st.p50), fmt_tput(st.tput), verdict(st.p50)};
}

// ---------------------------------------------------------------------------
// B: best_level_ptr() + consume_level_front() — single fill hot path
//
// This is the exact two-operation sequence the matching hot loop calls per
// fill iteration.  No account management.  The book is pre-seeded and the
// best ask is replenished after each fill.
// ---------------------------------------------------------------------------

static Row bench_fill_hotpath(int depth, int N) {
    OrderBook book;

    // Seed depth-1 asks (these persist throughout)
    for (int d = 1; d < depth; ++d) {
        auto o = make_order("SA" + std::to_string(d), OrderSide::SELL,
                             50000.0 + d, 1.0);
        book.add_order(o);
    }

    std::vector<uint64_t> s;
    s.reserve(N);

    for (int i = 0; i < N; ++i) {
        // Refill best ask so it can be consumed each iteration
        auto sell = make_order("RS" + std::to_string(i), OrderSide::SELL,
                                50000.0, 1.0);
        book.add_order(sell);   // not measured

        double best_price = 0.0;
        auto t0 = Clock::now();
        PriceLevel* lvl = book.best_level_ptr(OrderSide::BUY, best_price);
        if (lvl) {
            book.consume_level_front(OrderSide::BUY, lvl, 1.0);
        }
        s.push_back(elapsed_ns(t0));
    }

    auto st = compute(s);
    return Row{"B: fill hot path (best_level_ptr + consume)", std::to_string(depth) + " levels",
               fmt_ns(st.p50), fmt_tput(st.tput), verdict(st.p50)};
}

// ---------------------------------------------------------------------------
// C: remove_order() — O(1) cancel via stored iterator
//
// Book has `depth` levels with N orders spread across them.
// Cancels are shuffled to eliminate sequential-access bias.
// ---------------------------------------------------------------------------

static Row bench_cancel(int depth, int N) {
    OrderBook book;

    std::vector<std::string> oids;
    oids.reserve(N);

    for (int i = 0; i < N; ++i) {
        int    lv = i % depth;
        double px = 50000.0 + lv;
        std::string oid = "CO" + std::to_string(i);
        auto o = make_order(oid, OrderSide::SELL, px, 1.0);
        book.add_order(o);
        oids.push_back(oid);
    }

    std::mt19937 rng(42);
    std::shuffle(oids.begin(), oids.end(), rng);

    std::vector<uint64_t> s;
    s.reserve(N);

    for (auto& oid : oids) {
        auto t0 = Clock::now();
        book.remove_order(oid, 0.0, OrderSide::SELL);  // price/side ignored (O(1) via index)
        s.push_back(elapsed_ns(t0));
    }

    auto st = compute(s);
    std::string ds;
    if      (depth >= 10000) ds = "10,000 levels";
    else if (depth >= 1000)  ds = "1,000 levels";
    else                     ds = std::to_string(depth) + " levels";

    return Row{"C: remove_order (O(1) cancel)", ds,
               fmt_ns(st.p50), fmt_tput(st.tput), verdict(st.p50)};
}

// ---------------------------------------------------------------------------
// D: multi-level sweep — consume N levels in one tight loop
//
// Calls best_level_ptr() + consume_level_front() per level.
// Measures the raw loop cost — no engine overhead.
// ---------------------------------------------------------------------------

static Row bench_sweep(int levels, int N) {
    std::vector<uint64_t> s;
    s.reserve(N);

    for (int iter = 0; iter < N; ++iter) {
        OrderBook book;

        // Seed `levels` distinct ask levels
        for (int l = 0; l < levels; ++l) {
            auto o = make_order("SL" + std::to_string(iter * levels + l),
                                 OrderSide::SELL, 50000.0 + l, 1.0);
            book.add_order(o);
        }

        double remaining = static_cast<double>(levels);

        auto t0 = Clock::now();
        while (remaining > 1e-9) {
            double best_price = 0.0;
            PriceLevel* lvl = book.best_level_ptr(OrderSide::BUY, best_price);
            if (!lvl) break;

            double fill = std::min(remaining, lvl->orders.front().remaining_qty);
            book.consume_level_front(OrderSide::BUY, lvl, fill);
            remaining -= fill;
        }
        s.push_back(elapsed_ns(t0));
    }

    auto st = compute(s);
    double per_fill = (levels > 0) ? static_cast<double>(st.p50) / levels : 0.0;
    double fills_sec = (st.p50 > 0) ? (levels * 1e9 / st.p50) : 0.0;
    std::string ds = std::to_string(levels) + (levels == 1 ? " level" : " levels");
    uint64_t pf_u = static_cast<uint64_t>(per_fill);

    return Row{"D: sweep N levels (raw loop)", ds,
               fmt_ns(st.p50, levels > 1 ? per_fill : 0.0),
               fmt_tput(fills_sec, "fills/sec"),
               verdict(pf_u > 0 ? pf_u : st.p50)};
}

// ---------------------------------------------------------------------------
// E: deep FIFO fill — consume N orders queued at same price level
//
// All orders are at the same price.  Measures the per-node consumption
// cost inside a single price level (the inner FIFO loop).
// ---------------------------------------------------------------------------

static Row bench_deep_fifo(int k, int N) {
    std::vector<uint64_t> s;
    s.reserve(N);

    for (int iter = 0; iter < N; ++iter) {
        OrderBook book;

        for (int j = 0; j < k; ++j) {
            auto o = make_order("DF" + std::to_string(iter * k + j),
                                 OrderSide::SELL, 50000.0, 1.0);
            book.add_order(o);
        }

        double remaining = static_cast<double>(k);

        auto t0 = Clock::now();
        while (remaining > 1e-9) {
            double best_price = 0.0;
            PriceLevel* lvl = book.best_level_ptr(OrderSide::BUY, best_price);
            if (!lvl) break;
            double fill = std::min(remaining, lvl->orders.front().remaining_qty);
            book.consume_level_front(OrderSide::BUY, lvl, fill);
            remaining -= fill;
        }
        s.push_back(elapsed_ns(t0));
    }

    auto st = compute(s);
    double per_fill = (k > 0) ? static_cast<double>(st.p50) / k : 0.0;
    double fills_sec = (st.p50 > 0) ? (k * 1e9 / st.p50) : 0.0;
    std::string ds = std::to_string(k) + (k == 1 ? " order" : " orders");
    uint64_t pf_u = static_cast<uint64_t>(per_fill);

    return Row{"E: deep FIFO fill (same level)", ds,
               fmt_ns(st.p50, k > 1 ? per_fill : 0.0),
               fmt_tput(fills_sec, "fills/sec"),
               verdict(pf_u > 0 ? pf_u : st.p50)};
}

// ---------------------------------------------------------------------------
// F: available_qty() — FOK depth pre-check
//
// Iterates asks summing incremental qty.  O(L) price levels, O(1) per level.
// ---------------------------------------------------------------------------

static Row bench_available_qty(int depth, int N) {
    OrderBook book;

    for (int d = 0; d < depth; ++d) {
        auto o = make_order("SA" + std::to_string(d), OrderSide::SELL,
                             50000.0 + d, 1.0);
        book.add_order(o);
    }

    std::vector<uint64_t> s;
    s.reserve(N);

    double limit_px = 50000.0 + depth;  // check all levels

    for (int i = 0; i < N; ++i) {
        auto t0 = Clock::now();
        double qty = book.available_qty(OrderSide::BUY, limit_px);
        s.push_back(elapsed_ns(t0));
        (void)qty;
    }

    auto st = compute(s);
    return Row{"F: available_qty (FOK pre-check)", std::to_string(depth) + " levels",
               fmt_ns(st.p50), fmt_tput(st.tput), verdict(st.p50)};
}

// ---------------------------------------------------------------------------
// G: snapshot_json() — L2 depth snapshot
//
// Iterates top `snap_depth` levels and serialises to JSON string.
// ---------------------------------------------------------------------------

static Row bench_snapshot(int book_levels, int snap_depth, int N) {
    OrderBook book;

    for (int d = 0; d < book_levels; ++d) {
        auto bid = make_order("BID" + std::to_string(d), OrderSide::BUY,
                               49000.0 - d, 1.0);
        auto ask = make_order("ASK" + std::to_string(d), OrderSide::SELL,
                               51000.0 + d, 1.0);
        book.add_order(bid);
        book.add_order(ask);
    }

    std::vector<uint64_t> s;
    s.reserve(N);

    for (int i = 0; i < N; ++i) {
        auto t0 = Clock::now();
        std::string json = book.snapshot_json(snap_depth, static_cast<uint64_t>(i));
        s.push_back(elapsed_ns(t0));
        (void)json;
    }

    auto st = compute(s);
    std::string ds = std::to_string(snap_depth) + " levels snap";
    return Row{"G: snapshot_json (L2)", ds,
               fmt_ns(st.p50), fmt_tput(st.tput), verdict(st.p50)};
}

// ---------------------------------------------------------------------------
// Print table
// ---------------------------------------------------------------------------

static void print_table(const std::vector<Row>& rows) {
    const int W_OP    = 38;
    const int W_DEPTH = 16;
    const int W_LAT   = 28;
    const int W_TPUT  = 18;
    const int W_VRD   = 16;

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

    auto row = [&](const std::string& op,   const std::string& depth,
                    const std::string& lat,  const std::string& tput,
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

    std::string last_prefix;
    for (auto& r : rows) {
        // Group rows by first character (scenario letter)
        std::string prefix = r.op.substr(0, 2);
        std::string op_disp = (prefix == last_prefix) ? "" : r.op.substr(0, r.op.find('(') > 0 ? r.op.find('(') : 38);
        last_prefix = prefix;
        row(r.op, r.depth, r.latency, r.throughput, r.verd);
    }

    hline();
}

// ---------------------------------------------------------------------------
// Warmup
// ---------------------------------------------------------------------------

static void warmup() {
    OrderBook book;
    for (int i = 0; i < 20'000; ++i) {
        auto o = make_order("W" + std::to_string(i), OrderSide::SELL,
                             50000.0 + (i % 100), 1.0);
        book.add_order(o);
    }
    for (int i = 0; i < 20'000; ++i) {
        double best = 0.0;
        PriceLevel* lvl = book.best_level_ptr(OrderSide::BUY, best);
        if (lvl) book.consume_level_front(OrderSide::BUY, lvl, 1.0);
        else break;
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    std::cout << "+-------------------------------------------------------------------------+\n";
    std::cout << "|    Asgard Raw Order Book Benchmark  (no account/margin/sequencer)       |\n";
    std::cout << "|    Isolates the OrderBook data structure for apples-to-apples           |\n";
    std::cout << "|    comparison against reference engines                                 |\n";
    std::cout << "+-------------------------------------------------------------------------+\n\n";

    std::cout << "Warming up...\n";
    warmup();
    std::cout << "Running benchmarks...\n\n";

    std::vector<Row> results;

    // A: add_order at various book depths
    for (int d : {10, 100, 1000, 10000})
        results.push_back(bench_add(d, 100'000));

    // B: fill hot path (best_level_ptr + consume) at various depths
    for (int d : {10, 100, 1000})
        results.push_back(bench_fill_hotpath(d, 100'000));

    // C: cancel (remove_order) — O(1) should be flat vs depth
    for (int d : {10, 100, 1000, 10000})
        results.push_back(bench_cancel(d, 20'000));

    // D: multi-level sweep — raw loop, no engine overhead
    for (int l : {1, 5, 10, 50, 100})
        results.push_back(bench_sweep(l, 10'000));

    // E: deep FIFO fill — all at same price level
    for (int k : {1, 10, 50, 100})
        results.push_back(bench_deep_fifo(k, 10'000));

    // F: available_qty (FOK pre-check)
    for (int d : {10, 100, 1000})
        results.push_back(bench_available_qty(d, 100'000));

    // G: snapshot_json
    results.push_back(bench_snapshot(100, 10, 50'000));
    results.push_back(bench_snapshot(100, 25, 50'000));

    print_table(results);

    std::cout << "\nKey: These numbers reflect the raw book layer ONLY.\n";
    std::cout << "     Add ~100-200 ns for the full production pipeline (bench_latency).\n";
    std::cout << "     Cancel is O(1) via stored iterator — flat vs book depth.\n";

    return 0;
}
