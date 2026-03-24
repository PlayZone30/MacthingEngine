# FIFO Matching Engine — Implementation Plan

## Context
Build a production-grade FIFO (price-time priority) matching engine for Asgard FnO in **C++17**. Includes a full market simulation layer: GBM-based price movements, 10,000 automated trading users (retail, algo, market-maker profiles), and randomized bid/ask generation around the simulated market price. Exposes real **WebSocket + REST API** for external clients. Target throughput: ≥10,000 orders/sec.

---

## Issues / Gaps Found in the Spec (Fix Before Implementing)

| # | Issue | Resolution |
|---|-------|-----------|
| 1 | **Margin calibration bug**: 1% IM / 0.5% MM + 1% liquidation fee = guaranteed bankruptcy at max leverage | Use base_IMF=0.02 (2%), base_MMF=0.01 (1%) for simulation; note for production calibration |
| 2 | **FOK pre-scan**: Doc doesn't specify pre-scan vs attempt-then-cancel | Pre-scan opposite side for total available qty ≥ order qty before attempting; reject immediately if not enough |
| 3 | **Reduce-only partial fill**: Reduce-only sell of 5 when only long 3 | Fill min(order_qty, position_size)=3, cancel remaining 2 (would open a short) |
| 4 | **Stop orders during COOLDOWN**: Mark price engine keeps running but stops should freeze | Explicitly skip stop trigger scanner when instrument.state == COOLDOWN |
| 5 | **Modify during active liquidation**: Not addressed | Reject modifications when user's position_state == LIQUIDATING |
| 6 | **ADL execution price**: Listed as open question | Use mark_price at moment of ADL execution |
| 7 | **Position flip OI tracking**: Close 2 short + open 3 long = net +1 OI (not +3) | OI delta = abs(new_pos) - abs(old_pos) per user |
| 8 | **Mass quote atomicity**: One seq# but two book ops | Single seq# for the mass_quote; both book sides updated atomically before any new orders processed |

---

## Tech Stack

| Concern | Library | Reason |
|---------|---------|--------|
| WebSocket + HTTP server | **uWebSockets (uWS v20)** | Used by high-performance crypto exchanges; 0-copy, epoll-based; handles both WS and HTTP in one event loop |
| JSON serialization | **simdjson** (parse) + **nlohmann/json** (build) | simdjson is the fastest parser; nlohmann is ergonomic for building responses |
| Lock-free order queue | **moodycamel::ConcurrentQueue** | MPMC lock-free queue; user threads write orders, engine thread reads |
| Build system | **CMake 3.20+** | Standard C++ build |
| Testing | **Catch2** | Header-only, clean test syntax |
| Math | **STL `<cmath>`** | sqrt, exp for GBM; no heavy deps needed |

---

## Directory Structure

```
LNN_Crypto/
├── CMakeLists.txt
├── requirements/
│   └── (vcpkg.json or Conan conanfile.txt)
├── engine/
│   ├── models.hpp          # All structs, enums
│   ├── orderbook.hpp/cpp   # FIFO CLOB
│   ├── sequencer.hpp/cpp   # Monotonic seq# + WAL
│   ├── risk_checks.hpp/cpp # 7 pre-trade checks
│   ├── matching.hpp/cpp    # Core FIFO matching engine
│   ├── stop_orders.hpp/cpp # Stop trigger queue
│   ├── positions.hpp/cpp   # Position lifecycle
│   ├── margin.hpp/cpp      # Sqrt-based IMF/MMF
│   ├── risk_monitor.hpp/cpp # Continuous margin check
│   ├── liquidation.hpp/cpp # Gradual liquidation + vault + ADL
│   ├── mark_price.hpp/cpp  # Index + 1-min EWMA ±5% bound
│   ├── circuit_breakers.hpp/cpp # 4-layer protection
│   └── insurance_vault.hpp/cpp  # Open vault + utilization gate
├── simulation/
│   ├── market_price.hpp/cpp  # GBM price simulator
│   ├── user_profiles.hpp     # Profile configs
│   ├── user_simulator.hpp/cpp # 10,000 user threads
│   └── order_generator.hpp/cpp
├── api/
│   ├── websocket_server.hpp/cpp  # uWS WebSocket handler
│   ├── rest_server.hpp/cpp       # uWS HTTP handler
│   ├── subscriptions.hpp/cpp     # Per-stream subscription lists
│   └── broadcaster.hpp/cpp       # Fan-out to WebSocket clients
├── stats/
│   └── stats_collector.hpp/cpp
├── main.cpp                # Orchestrator
└── tests/
    ├── test_orderbook.cpp
    ├── test_matching.cpp
    ├── test_risk.cpp
    └── test_margin.cpp
```

---

## Phase 1 — Data Models (`engine/models.hpp`)

All types. Header-only, no logic.

```cpp
enum class OrderType    { LIMIT, MARKET, STOP_LIMIT, POST_ONLY };
enum class OrderSide    { BUY, SELL };
enum class TIF          { GTC, IOC, FOK };
enum class STPMode      { CANCEL_INCOMING, CANCEL_RESTING, CANCEL_BOTH };
enum class OrderStatus  { NEW, OPEN, PARTIAL, FILLED, CANCELLED, REJECTED };
enum class InstrumentState { PRE_OPEN, TRADING, COOLDOWN, CLOSE_ONLY, SETTLING, SETTLED, EMERGENCY };
enum class MarginMode   { CROSS, ISOLATED };
enum class PositionSide { LONG, SHORT };
enum class PositionState { OPEN, LIQUIDATING, CLOSED };

struct Order {
    std::string   order_id;
    std::string   user_id;
    std::string   instrument;
    OrderSide     side;
    OrderType     type;
    double        price;          // 0 for market orders
    double        quantity;
    double        remaining_qty;
    TIF           tif;
    bool          reduce_only;
    STPMode       stp_mode;
    MarginMode    margin_mode;
    uint64_t      seq;
    uint64_t      timestamp_us;   // microseconds since epoch
    OrderStatus   status;
    double        arrival_best_price; // for Layer 2 impact band
};

struct Trade {
    std::string  trade_id;
    std::string  instrument;
    double       price;
    double       quantity;
    std::string  buyer_id, seller_id;
    std::string  buyer_order_id, seller_order_id;
    bool         buyer_is_taker;
    uint64_t     timestamp_us;
    uint64_t     seq;
};

struct Position {
    std::string   user_id;
    std::string   instrument;
    PositionSide  side;
    double        size;
    double        entry_price;
    MarginMode    margin_mode;
    double        allocated_margin; // isolated mode only
    PositionState state;
};

struct Instrument {
    std::string     symbol;
    InstrumentState state;
    double  min_lot, max_lot, lot_step;
    double  price_band_pct;     // Layer 1: ±X% around mark
    double  impact_band_pct;    // Layer 2: max fill deviation from arrival best
    double  velocity_threshold; // Layer 3: % move in velocity_window triggers cooldown
    int     velocity_window_s;
    int     cooldown_duration_s;
    double  base_imf, imf_factor; // sqrt margin params
    double  base_mmf, mmf_factor;
    double  max_position_size;
};

struct UserAccount {
    std::string  user_id;
    double       wallet_balance;
    double       open_order_margin;
    std::unordered_map<std::string, Position>  positions;     // instrument -> Position
    std::unordered_map<std::string, Order>     open_orders;   // order_id -> Order
    int          rate_limit_per_sec;
    std::atomic<int> messages_this_second{0};
    std::string  user_type; // "RETAIL", "ALGO", "MM"
};

struct InsuranceVault {
    double current_balance;
    double total_losses_absorbed;
    double sitg_balance; // permanently locked exchange capital
};
```

---

## Phase 2 — Order Book (`engine/orderbook.hpp/cpp`)

FIFO CLOB: `std::map` for price level index (O(log n)), `std::deque` per level for FIFO.

```cpp
class OrderBook {
    // Bids: highest price first → use std::map with reverse iteration or std::map<neg_price>
    std::map<double, std::deque<Order>, std::greater<double>> bids; // desc
    std::map<double, std::deque<Order>>                        asks; // asc

public:
    void add_order(const Order& o);
    void remove_order(const std::string& order_id, double price, OrderSide side);
    std::optional<double> best_bid() const;
    std::optional<double> best_ask() const;
    // Returns deque front (FIFO head) without removing
    Order* peek_best(OrderSide opposite_side);
    // Remove front of level
    void consume_front(OrderSide side, double price, double qty_consumed);
    // L2 snapshot for market data
    nlohmann::json snapshot(int depth = 100) const;
    // Total available qty at-or-better than price on opposite side (for FOK pre-check)
    double available_qty(OrderSide aggressive_side, double price_limit) const;
};
```

Key: `bids` uses `std::greater<double>` comparator so `begin()` gives best bid. `asks` uses default so `begin()` gives best ask.

---

## Phase 3 — Sequencer + WAL (`engine/sequencer.hpp/cpp`)

```cpp
class Sequencer {
    std::atomic<uint64_t> counter_{0};
    std::ofstream wal_file_;          // append-only WAL (optional, file-based)
    std::mutex    wal_mutex_;

public:
    uint64_t next(const std::string& event_type, const std::string& payload_json);
    void replay(std::function<void(uint64_t, std::string, std::string)> handler);
};
```

`std::atomic<uint64_t>` with `fetch_add(1, memory_order_relaxed)` for the sequence counter. WAL written per-event for crash recovery. In simulation mode, WAL is in-memory (`std::vector`).

---

## Phase 4 — Pre-Trade Risk Checks (`engine/risk_checks.hpp/cpp`)

Seven checks, fail-fast. Returns `{pass, reason}`.

```cpp
struct RiskResult { bool pass; std::string reason; };

RiskResult check_all(const Order& o, const UserAccount& user,
                     const Instrument& inst, double mark_price) {
    // 1. Instrument state
    if (inst.state != InstrumentState::TRADING)
        return {false, "Instrument not available"};

    // 2. Price band (limit orders only)
    if (o.type == OrderType::LIMIT) {
        if (o.side == OrderSide::BUY  && o.price > mark_price * (1 + inst.price_band_pct))
            return {false, "Price above limit band"};
        if (o.side == OrderSide::SELL && o.price < mark_price * (1 - inst.price_band_pct))
            return {false, "Price below limit band"};
    }

    // 3. Size validation
    if (o.quantity < inst.min_lot)  return {false, "Below min lot"};
    if (o.quantity > inst.max_lot)  return {false, "Above max lot"};
    if (std::fmod(o.quantity, inst.lot_step) > 1e-9) return {false, "Not whole lots"};

    // 4. Rate limit
    if (user.messages_this_second.load() >= user.rate_limit_per_sec)
        return {false, "Rate limit exceeded"};

    // 5. Position limit
    double hyp = current_net_position(user, o.instrument)
               + (o.side == OrderSide::BUY ? 1.0 : -1.0) * o.quantity;
    if (std::abs(hyp) > inst.max_position_size)
        return {false, "Position limit exceeded"};

    // 6. Margin (sqrt-based)
    double add_im = compute_additional_im(o, user, inst, mark_price);
    if (available_balance(user, mark_price) < add_im)
        return {false, "Insufficient margin"};

    // 7. STP: evaluated inside matching engine at match time
    return {true, ""};
}
```

`compute_additional_im` logic:
- **Increases position**: `IMF(notional_of_order) × notional_of_order`
- **Reduces position**: `0.0`
- **Flips position**: `IM_new_direction - IM_freed_from_close`
- **Reduce-only**: also validate `o.quantity ≤ position.size`

---

## Phase 5 — Matching Engine (`engine/matching.hpp/cpp`)

Core hot path. Processes one order sequentially on the engine thread.

```cpp
std::vector<Trade> MatchingEngine::process(Order& incoming) {
    std::vector<Trade> trades;
    auto& opp_book = (incoming.side == BUY) ? asks_ : bids_;

    // Record arrival best price for Layer 2 impact band
    incoming.arrival_best_price = (incoming.side == BUY)
        ? book_.best_ask().value_or(0.0)
        : book_.best_bid().value_or(std::numeric_limits<double>::max());

    // FOK pre-check
    if (incoming.tif == TIF::FOK) {
        double avail = book_.available_qty(incoming.side, incoming.price);
        if (avail < incoming.quantity) { cancel(incoming); return {}; }
    }

    // Post-Only: reject if would cross
    if (incoming.type == OrderType::POST_ONLY && would_cross(incoming)) {
        reject(incoming, "Would match immediately"); return {};
    }

    // Matching loop
    while (incoming.remaining_qty > 0.0) {
        auto best_opt = (incoming.side == BUY) ? book_.best_ask() : book_.best_bid();
        if (!best_opt) break;
        double best_price = *best_opt;

        // Price crosses?
        bool crosses = (incoming.side == BUY)
            ? (incoming.price >= best_price || incoming.type == OrderType::MARKET)
            : (incoming.price <= best_price || incoming.type == OrderType::MARKET);
        if (!crosses) break;

        // Layer 2: impact band check
        if (violates_impact_band(best_price, incoming.arrival_best_price,
                                  inst_.impact_band_pct, incoming.side)) break;

        Order& resting = *book_.peek_best_order(incoming.side == BUY ? SELL : BUY, best_price);

        // STP
        if (resting.user_id == incoming.user_id) {
            apply_stp(incoming, resting); continue;
        }

        // Reduce-only constraint on incoming
        double max_fill = incoming.reduce_only
            ? std::min(incoming.remaining_qty, net_position_size(incoming.user_id, incoming.instrument))
            : incoming.remaining_qty;
        if (max_fill <= 0) break;

        double fill_qty = std::min(max_fill, resting.remaining_qty);

        // Emit trade
        Trade t;
        t.price = best_price; t.quantity = fill_qty;
        t.buyer_id    = (incoming.side == BUY) ? incoming.user_id : resting.user_id;
        t.seller_id   = (incoming.side == BUY) ? resting.user_id  : incoming.user_id;
        t.buyer_is_taker = (incoming.side == BUY);
        t.timestamp_us = now_us();
        t.seq = sequencer_.next("TRADE", to_json(t));
        trades.push_back(t);

        incoming.remaining_qty -= fill_qty;
        resting.remaining_qty  -= fill_qty;
        if (resting.remaining_qty < 1e-9) book_.remove_resting(resting);
    }

    // Handle residual
    if (incoming.remaining_qty > 1e-9) {
        if (incoming.tif == TIF::GTC) {
            book_.add_order(incoming);
            reserve_open_order_margin(incoming);
        } else {
            cancel(incoming); // IOC, FOK, MARKET
        }
    }
    return trades;
}
```

---

## Phase 6 — Post-Trade Processing (`engine/positions.cpp`)

Four cases. Called once per Trade, twice (once for each side).

```cpp
double apply_trade_to_position(UserAccount& user, const Trade& t, bool is_buyer) {
    PositionSide trade_side = is_buyer ? PositionSide::LONG : PositionSide::SHORT;
    double realized_pnl = 0.0;
    auto it = user.positions.find(t.instrument);

    if (it == user.positions.end()) {             // Case A: open new
        user.positions[t.instrument] = Position{user.user_id, t.instrument,
            trade_side, t.quantity, t.price, MarginMode::CROSS, 0.0, PositionState::OPEN};
    } else {
        Position& pos = it->second;
        if (pos.side == trade_side) {             // Case B: increase
            double new_entry = (pos.entry_price * pos.size + t.price * t.quantity)
                              / (pos.size + t.quantity);
            pos.size += t.quantity;
            pos.entry_price = new_entry;
        } else if (t.quantity < pos.size) {       // Case C: partial reduce
            realized_pnl = (t.price - pos.entry_price) * t.quantity
                         * (pos.side == PositionSide::LONG ? 1.0 : -1.0);
            user.wallet_balance += realized_pnl;
            pos.size -= t.quantity;
        } else {                                  // Case D: close + flip
            double close_qty = pos.size;
            realized_pnl = (t.price - pos.entry_price) * close_qty
                          * (pos.side == PositionSide::LONG ? 1.0 : -1.0);
            user.wallet_balance += realized_pnl;
            double flip_qty = t.quantity - close_qty;
            if (flip_qty > 1e-9)
                pos = Position{user.user_id, t.instrument, trade_side,
                               flip_qty, t.price, pos.margin_mode, 0.0, PositionState::OPEN};
            else
                user.positions.erase(it);
        }
    }

    // Release open_order_margin for the filled portion
    release_order_margin(user, t);
    // Deduct fee
    double fee_rate = is_taker(t, is_buyer) ? 0.0005 : 0.0002;
    user.wallet_balance -= t.quantity * t.price * fee_rate;

    return realized_pnl;
}
```

Zero-sum invariant check (runs every 1000 trades):
```cpp
// Σ(wallet_balance) + Σ(unrealized_pnl) + fee_revenue + vault_balance == total_deposited
assert(std::abs(sum_all_equity() + fee_revenue_ + vault_.current_balance
                - total_deposited_) < 0.01);
```

---

## Phase 7 — Margin Engine (`engine/margin.hpp/cpp`)

```cpp
double imf(double notional, double base, double factor) {
    return std::max(base, factor * std::sqrt(notional));
}

double position_margin(const Position& pos, double mark_price, const Instrument& inst) {
    double notional = pos.size * mark_price;
    return imf(notional, inst.base_imf, inst.imf_factor) * notional;
}

double unrealized_pnl(const Position& pos, double mark_price) {
    double dir = (pos.side == PositionSide::LONG) ? 1.0 : -1.0;
    return dir * (mark_price - pos.entry_price) * pos.size;
}

double equity(const UserAccount& user, double mark_price) {
    double upnl = 0;
    for (auto& [sym, pos] : user.positions)
        upnl += unrealized_pnl(pos, mark_price);
    return user.wallet_balance + upnl;
}

double available_balance(const UserAccount& user, double mark_price, const Instrument& inst) {
    double eq = equity(user, mark_price);
    double pm = 0;
    for (auto& [sym, pos] : user.positions)
        pm += position_margin(pos, mark_price, inst);
    return eq - pm - user.open_order_margin;
}

double liq_price_estimate(const Position& pos, const UserAccount& user,
                           double mark_price, const Instrument& inst) {
    double mm = imf(pos.size * mark_price, inst.base_mmf, inst.mmf_factor) * pos.size * mark_price;
    double eq = equity(user, mark_price);
    double dir = (pos.side == PositionSide::LONG) ? -1.0 : 1.0;
    return pos.entry_price + dir * (eq - mm) / pos.size;
}
```

---

## Phase 8 — Mark Price Engine (`engine/mark_price.hpp/cpp`)

```cpp
class MarkPriceEngine {
    double ewma_ = 0.0;
    static constexpr double alpha_ = 0.0116; // 1-min halflife, per-second

public:
    double update(double index_price, double mid_price) {
        double basis = mid_price - index_price;
        ewma_ = alpha_ * basis + (1.0 - alpha_) * ewma_;
        double mark = index_price + ewma_;
        mark = std::clamp(mark, index_price * 0.95, index_price * 1.05);
        return mark;
    }
};
```

Simulation: GBM price → `index_price`. `mid_price` = `(best_bid + best_ask) / 2` from order book.

---

## Phase 9 — Stop Order Trigger Queue (`engine/stop_orders.hpp/cpp`)

```cpp
void StopTriggerQueue::on_mark_price_update(double mark_price,
                                             const Instrument& inst,
                                             std::function<void(Order)> inject) {
    if (inst.state == InstrumentState::COOLDOWN) return; // frozen

    std::vector<StopOrder> triggered;
    for (auto& stop : stops_) {
        if (stop.trigger_source == TriggerSource::MARK) {
            bool fires = (stop.side == OrderSide::BUY  && mark_price >= stop.trigger_price)
                      || (stop.side == OrderSide::SELL && mark_price <= stop.trigger_price);
            if (fires) triggered.push_back(stop);
        }
    }
    for (auto& s : triggered) {
        inject(s.to_limit_order()); // → sequencer → risk → matching engine
        stops_.erase(std::remove(stops_.begin(), stops_.end(), s), stops_.end());
    }
}
```

Margin reserved at stop placement. Reduce-only stops: no IM reserved.

---

## Phase 10 — Liquidation Engine (`engine/liquidation.hpp/cpp`)

```cpp
void LiquidationEngine::trigger(UserAccount& user, const Instrument& inst, double mark_price) {
    user.positions.at(inst.symbol).state = PositionState::LIQUIDATING;

    // Step 1: cancel all open orders
    for (auto& [id, order] : user.open_orders)
        matching_engine_.cancel(order);
    user.open_orders.clear();
    user.open_order_margin = 0.0;

    // Step 2: re-check
    if (equity(user, mark_price) > total_mm(user, mark_price, inst)) {
        user.positions.at(inst.symbol).state = PositionState::OPEN;
        return;
    }

    // Step 3: select position (largest unrealized loss)
    Position& pos = select_worst_position(user, mark_price);

    // Step 4-7: gradual loop
    while (pos.size > 1e-9) {
        double chunk = std::max(inst.min_lot, pos.size * 0.10);

        // Liquidation order: reduce-only IOC with price band
        double liq_limit = (pos.side == PositionSide::LONG)
            ? mark_price * (1.0 - 0.05)   // selling: 5% below mark
            : mark_price * (1.0 + 0.05);  // buying:  5% above mark

        Order liq_order = make_liq_order(user, inst, pos, chunk, liq_limit);
        auto trades = matching_engine_.process(liq_order);

        for (auto& t : trades) {
            double liq_fee = t.quantity * t.price * 0.01;
            user.wallet_balance -= liq_fee;
            vault_.credit(liq_fee);

            // Bankruptcy check
            double bankrupt_price = (pos.side == PositionSide::LONG)
                ? pos.entry_price - (pos.allocated_margin / pos.size)
                : pos.entry_price + (pos.allocated_margin / pos.size);

            bool bankrupt = (pos.side == PositionSide::LONG)
                ? t.price < bankrupt_price
                : t.price > bankrupt_price;

            if (bankrupt) {
                double deficit = std::abs(t.price - bankrupt_price) * t.quantity;
                if (!vault_.absorb_deficit(deficit))
                    adl_engine_.trigger(pos, mark_price);
            }
        }

        apply_trade_to_position(user, trades, ...);

        if (equity(user, mark_price) > total_mm(user, mark_price, inst)) break;
    }
    pos.state = (pos.size < 1e-9) ? PositionState::CLOSED : PositionState::OPEN;
}
```

**Insurance vault utilization:**
```cpp
double InsuranceVault::utilization() const {
    double original = current_balance + total_losses_absorbed;
    return (original > 0) ? total_losses_absorbed / original : 0.0;
}
```

---

## Phase 11 — Circuit Breakers (`engine/circuit_breakers.hpp/cpp`)

```cpp
class VelocityDetector {
    std::deque<std::pair<int64_t, double>> history_; // (timestamp_s, price)
    int window_s_;
    double threshold_;
public:
    bool update(double mark_price, int64_t now_s) {
        history_.push_back({now_s, mark_price});
        while (!history_.empty() && history_.front().first < now_s - window_s_)
            history_.pop_front();
        if (history_.size() < 2) return false;
        double oldest = history_.front().second;
        double move = std::abs(mark_price - oldest) / oldest;
        return move > threshold_;
    }
};

void CircuitBreakerManager::check(Instrument& inst, double mark_price,
                                   const InsuranceVault& vault, int64_t now_s) {
    // Layer 3: velocity per instrument
    if (velocity_detectors_[inst.symbol].update(mark_price, now_s)) {
        inst.state = InstrumentState::COOLDOWN;
        cooldown_expires_[inst.symbol] = now_s + inst.cooldown_duration_s;
    }
    // Layer 3: expire cooldown
    if (inst.state == InstrumentState::COOLDOWN &&
        now_s >= cooldown_expires_[inst.symbol])
        inst.state = InstrumentState::TRADING;

    // Layer 4: market-wide
    int cooldown_count = count_cooldowns(all_instruments_);
    if (cooldown_count >= (int)all_instruments_.size() ||
        vault.utilization() > 0.90)
        set_emergency_all(); // admin must manually recover
}
```

Layer 1 is in `risk_checks.cpp`. Layer 2 is inside `matching.cpp` (impact band per fill).

---

## Phase 12 — Market Price Simulator (`simulation/market_price.hpp/cpp`)

GBM: `dS = μ·S·dt + σ·S·dW`

```cpp
class GBMSimulator {
    double price_  = 50'000.0;
    double mu_     = 0.0;    // no drift for stress test
    double sigma_  = 0.60;   // 60% annual vol ≈ 0.038% per 100ms
    double dt_     = 0.1;    // 100ms tick
    std::mt19937_64 rng_{std::random_device{}()};
    std::normal_distribution<double> dist_{0.0, 1.0};

public:
    double tick() {
        double z = dist_(rng_);
        price_ *= std::exp((mu_ - 0.5 * sigma_ * sigma_) * dt_
                          + sigma_ * std::sqrt(dt_) * z);
        return price_;
    }
    double price() const { return price_; }
};
```

---

## Phase 13 — User Simulator (`simulation/user_simulator.hpp/cpp`)

10,000 users across 3 profiles. Each runs in a thread and pushes orders into the lock-free queue.

| Profile | Count | Arrival Rate | Order Types | Price Offset |
|---------|-------|-------------|-------------|--------------|
| Retail  | 8,000 | Poisson λ=0.05/s | 70% limit, 25% market, 5% stop | ±0-2% of market |
| Algo    | 1,500 | λ=0.5/s | 50% limit, 40% market, 10% stop | ±0-0.5% |
| MM      |   500 | λ=5/s both sides | 99% limit (bid+ask pairs) | ±spread/2 (0.01-0.05%) |

```cpp
void run_user(UserAccount& user, GBMSimulator& market,
              moodycamel::ConcurrentQueue<Order>& queue) {
    while (running_) {
        double sleep_ms = sample_inter_arrival(user.user_type);
        std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(sleep_ms));

        double mp = market.price();
        Order o = generate_order(user, mp, user.user_type);
        queue.enqueue(o);
    }
}
```

Each user account: wallet_balance (log-normal 1,000–100,000 USDC), initial positions = empty.

---

## Phase 14 — WebSocket + REST API (`api/`)

**uWebSockets setup:**

```cpp
// websocket_server.cpp
uWS::App().ws<PerSocketData>("/*", {
    .open = [](auto* ws) { /* authenticate, register */ },
    .message = [](auto* ws, std::string_view msg, uWS::OpCode) {
        auto j = nlohmann::json::parse(msg);
        if (j["method"] == "SUBSCRIBE")
            subscription_manager.subscribe(ws, j["params"]);
        else if (j["method"] == "place_order")
            order_queue.enqueue(parse_order(j));
    },
    .close = [](auto* ws, int, std::string_view) {
        subscription_manager.unsubscribe_all(ws);
        cancel_on_disconnect_if_mm(ws);
    }
}).get("/api/v1/depth", [](auto* res, auto* req) {
    auto snap = orderbook.snapshot(100);
    res->end(snap.dump());
}).get("/api/v1/ticker", [](auto* res, auto* req) {
    res->end(ticker.to_json().dump());
}).listen(9001, ...).run();
```

**WebSocket channels:**

| Public | Private (authed) |
|--------|-----------------|
| `depth.<instrument>` | `account.orders` |
| `trade.<instrument>` | `account.positions` |
| `ticker.<instrument>` | `account.balances` |
| `markPrice.<instrument>` | `account.warnings` |
| `kline.<interval>.<instrument>` | |
| `liquidation.<instrument>` | |

**Broadcaster fan-out** (called after each trade/book update):

```cpp
void Broadcaster::publish(const std::string& topic, const nlohmann::json& data) {
    auto subs = subscriptions_.get_subscribers(topic);
    auto payload = nlohmann::json{{"stream", topic}, {"data", data}}.dump();
    for (auto* ws : subs)
        ws->send(payload, uWS::OpCode::TEXT);
}
```

---

## Phase 15 — Orchestrator (`main.cpp`)

```cpp
int main() {
    Instrument btc_inst = make_btc_instrument();
    GBMSimulator market;
    moodycamel::ConcurrentQueue<Order> order_queue(200'000);
    Sequencer sequencer;
    OrderBook book;
    MatchingEngine engine(book, sequencer, btc_inst);
    RiskMonitor risk_monitor;
    LiquidationEngine liq_engine(engine, vault);
    MarkPriceEngine mark_engine;
    CircuitBreakerManager cb_manager;
    StatsCollector stats;

    // Start 10,000 user threads
    std::vector<std::thread> user_threads;
    for (auto& user : users)
        user_threads.emplace_back(run_user, std::ref(user), std::ref(market), std::ref(order_queue));

    // Market price tick thread (10 ticks/sec)
    std::thread market_thread([&] {
        while (true) {
            auto idx_price = market.tick();
            auto mid = book.mid_price();
            double mark = mark_engine.update(idx_price, mid);
            stop_queue.on_mark_price_update(mark, btc_inst, [&](Order o){ order_queue.enqueue(o); });
            risk_monitor.check_all(users, mark, btc_inst, liq_engine);
            cb_manager.check(btc_inst, mark, vault, now_s());
            broadcaster.publish("markPrice.BTC-USDC-20260327", mark_json(mark, idx_price));
            std::this_thread::sleep_for(100ms);
        }
    });

    // Engine loop (single-threaded, processes queue)
    std::thread engine_thread([&] {
        Order o;
        while (true) {
            while (order_queue.try_dequeue(o)) {
                o.seq = sequencer.next("NEW_ORDER", to_json(o));
                auto [pass, reason] = risk_checks::check_all(o, users[o.user_id], btc_inst, mark_engine.mark());
                if (!pass) { stats.record_rejection(reason); continue; }
                auto trades = engine.process(o);
                for (auto& t : trades) {
                    apply_trade_to_position(users[t.buyer_id], t, true);
                    apply_trade_to_position(users[t.seller_id], t, false);
                    stats.record_trade(t);
                    broadcaster.publish("trade.BTC-USDC-20260327", trade_json(t));
                }
                broadcaster.publish("depth.BTC-USDC-20260327", book.snapshot(20));
                stats.record_order(o);
            }
        }
    });

    // uWS runs on main thread
    start_websocket_rest_server();
}
```

---

## Phase 16 — Statistics (`stats/stats_collector.hpp/cpp`)

Reported every 10 seconds to terminal + pushed via WebSocket to any connected monitoring client:

- **Throughput**: orders/sec, trades/sec, fill rate (%)
- **Latency**: histogram of `order.timestamp_us → trade.timestamp_us` (p50, p95, p99)
- **P&L**: top 10 winners, top 10 losers, total system P&L (should be ~0)
- **Liquidations**: count, notional liquidated, vault balance + utilization
- **Circuit breakers**: COOLDOWN triggers, EMERGENCY triggers
- **Book state**: best bid/ask, spread, depth at top 5 levels
- **Zero-sum check**: Σ(equity) + fee_revenue + vault_balance - total_deposited < $0.01

---

## Build System (`CMakeLists.txt`)

```cmake
cmake_minimum_required(VERSION 3.20)
project(asgard_matching_engine CXX)
set(CMAKE_CXX_STANDARD 17)

find_package(uWebSockets REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(Catch2 REQUIRED)

add_executable(engine
    main.cpp
    engine/orderbook.cpp engine/sequencer.cpp engine/risk_checks.cpp
    engine/matching.cpp  engine/positions.cpp engine/margin.cpp
    engine/risk_monitor.cpp engine/liquidation.cpp engine/mark_price.cpp
    engine/circuit_breakers.cpp engine/stop_orders.cpp engine/insurance_vault.cpp
    simulation/market_price.cpp simulation/user_simulator.cpp simulation/order_generator.cpp
    api/websocket_server.cpp api/broadcaster.cpp
    stats/stats_collector.cpp)

target_link_libraries(engine uWebSockets::uWebSockets nlohmann_json::nlohmann_json)

# Tests
add_executable(tests tests/test_orderbook.cpp tests/test_matching.cpp
                     tests/test_risk.cpp tests/test_margin.cpp)
target_link_libraries(tests Catch2::Catch2WithMain ...)
```

---

## Verification Checklist

| Test | How to verify |
|------|--------------|
| Throughput ≥10k orders/sec | Run 60 sec, check `stats.orders_per_sec` |
| FIFO correctness | Place 10 orders same price different users → fills in submission order |
| Zero-sum invariant | After every 1000 trades, assert sum < $0.01 |
| FOK cancel | FOK with insufficient depth → entire order cancelled, no partial fill |
| IOC partial fill | IOC sweeps available qty, rest cancelled |
| Post-Only reject | Post-Only order that crosses → rejected (not CANCELLED, REJECTED) |
| Reduce-only partial | Reduce-only sell qty > position → fills up to position size |
| STP all 3 modes | Same-user cross with CANCEL_INCOMING / CANCEL_RESTING / CANCEL_BOTH |
| Modify loses priority | Modify GTC order → new seq#, goes to back of queue |
| Gradual liquidation | Force equity below MM → 10% chunks, stops when restored |
| Circuit breaker L3 | Simulate 8% price move in 60s → COOLDOWN, market orders blocked |
| Sqrt margin formula | 1 BTC at $50k: IMF = max(0.02, 0.00003×√50000) = 0.02 ✓ |
| Stop frozen in cooldown | Stop trigger price crossed during COOLDOWN → no injection |
| Reduce-only stop no IM | Place reduce-only stop → open_order_margin unchanged |
| WebSocket stream | Subscribe to `depth.BTC-USDC-20260327` → receive incremental updates with seq# |
| REST snapshot | `GET /api/v1/depth` → returns L2 JSON with bids/asks arrays |

---

## Production Upgrade Path

- **WAL**: Replace in-memory vector with `O_APPEND` file or Kafka topic
- **Mark price oracle**: Replace GBM with Pyth/Switchboard websocket feed
- **TLS**: Add uWS TLS config for production endpoints
- **Horizontal scaling**: Add Redis pub/sub for multi-node WebSocket fan-out
- **FIX gateway**: Add FIX 4.2/4.4 protocol layer for institutional MMs (V1.1)
