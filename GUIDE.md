# Asgard FnO — Matching Engine Guide

Complete reference for building, running, using the APIs, and understanding the system internals.

---

## Table of Contents

1. [Build & Run](#1-build--run)
2. [Architecture Overview](#2-architecture-overview)
3. [Thread Model](#3-thread-model)
4. [Order Lifecycle (Full Flow)](#4-order-lifecycle-full-flow)
5. [Pre-Trade Risk Checks](#5-pre-trade-risk-checks)
6. [Matching Engine (FIFO CLOB)](#6-matching-engine-fifo-clob)
7. [Post-Trade Processing](#7-post-trade-processing)
8. [Margin System](#8-margin-system)
9. [Mark Price & GBM Simulation](#9-mark-price--gbm-simulation)
10. [Stop Orders](#10-stop-orders)
11. [Liquidation Engine](#11-liquidation-engine)
12. [Circuit Breakers](#12-circuit-breakers)
13. [Insurance Vault & ADL](#13-insurance-vault--adl)
14. [WebSocket API](#14-websocket-api)
15. [REST API](#15-rest-api)
16. [Public Streams Reference](#16-public-streams-reference)
17. [Private Streams Reference](#17-private-streams-reference)
18. [WebSocket Message Examples](#18-websocket-message-examples)
19. [Simulation Layer](#19-simulation-layer)
20. [Statistics & Zero-Sum Check](#20-statistics--zero-sum-check)
21. [Test Suite](#21-test-suite)

---

## 1. Build & Run

### Prerequisites

- CMake ≥ 3.20
- C++17 compiler (clang++ on macOS, g++ on Linux)
- Internet connection (first build downloads deps via FetchContent)
- `zlib` (comes with macOS/Xcode; on Linux: `libz-dev`)
- `pthread`

### Build

```bash
# From the project root
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

First configure takes ~15 minutes to download:
- `Catch2 v3.5.4`
- `nlohmann/json v3.11.3`
- `moodycamel/concurrentqueue v1.0.4`
- `uSockets v0.8.8`
- `uWebSockets v20.62.0`

Subsequent configures use the CMake cache and take ~2 seconds.

### Run the simulation

```bash
# Default: 10,000 simulated users, WebSocket on port 9001
./build/sim

# Custom: 500 users, port 8080
./build/sim 500 8080
```

Stop with `Ctrl-C` (SIGINT). Final stats are printed on shutdown.

### Run the tests

```bash
cd build
ctest --output-on-failure
# or run directly:
./tests
```

Expected output:
```
All tests passed (76 assertions in 40 test cases)
```

---

## 2. Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                          main.cpp                               │
│                                                                 │
│  ┌────────────┐  ┌──────────────┐  ┌──────────┐  ┌──────────┐  │
│  │  Market    │  │   Engine     │  │  Stats   │  │  uWS     │  │
│  │  Thread    │  │   Thread     │  │  Thread  │  │  (main)  │  │
│  │  100ms tick│  │  drain queue │  │  10s     │  │  WS+REST │  │
│  └─────┬──────┘  └──────┬───────┘  └──────────┘  └────┬─────┘  │
│        │                │                               │        │
│   GBMSimulator    ConcurrentQueue ◄──────────────────── │        │
│   MarkPriceEngine      │          (WS place_order)      │        │
│   StopTriggerQueue     │                               │        │
│   RiskMonitor          ▼                               │        │
│   CircuitBreakers  RiskEngine                          │        │
│                    MatchingEngine ──► Broadcaster ─────►│        │
│                    PositionUpdater                      │        │
│                    LiquidationEngine                   │        │
└─────────────────────────────────────────────────────────────────┘
```

### Core Components

| Component | File | Role |
|-----------|------|------|
| `OrderBook` | `engine/orderbook.cpp` | FIFO CLOB — price/time priority |
| `Sequencer` | `engine/sequencer.cpp` | Monotonic seq# + in-memory WAL |
| `RiskEngine` | `engine/risk_checks.cpp` | 8 pre-trade checks |
| `MatchingEngine` | `engine/matching.cpp` | FIFO matching, STP, impact band |
| `MarkPriceEngine` | `engine/mark_price.hpp` | EWMA mark price, clamped ±5% |
| `StopTriggerQueue` | `engine/stop_orders.cpp` | Stop-limit trigger scanner |
| `RiskMonitor` | `engine/risk_monitor.cpp` | Per-tick margin health scan |
| `LiquidationEngine` | `engine/liquidation.cpp` | Gradual 10% chunk liquidation |
| `CircuitBreakerManager` | `engine/circuit_breakers.cpp` | 4-layer market protection |
| `InsuranceVault` | `engine/insurance_vault.hpp` | Socialised loss cover + ADL |
| `GBMSimulator` | `simulation/market_price.hpp` | Geometric Brownian Motion price |
| `UserSimulator` | `simulation/user_simulator.cpp` | 10,000 synthetic traders |
| `WebSocketServer` | `api/websocket_server.cpp` | uWS WS + REST |
| `Broadcaster` | `api/broadcaster.cpp` | Fan-out to WS subscribers |
| `StatsCollector` | `stats/stats_collector.cpp` | Throughput, latency, PnL |

---

## 3. Thread Model

| Thread | What it does | Sleep |
|--------|-------------|-------|
| **Market thread** | GBM tick → mark price → stop triggers → risk monitor scan → circuit breaker update → broadcast markPrice | 100 ms |
| **Engine thread** | Drain `ConcurrentQueue` in batches of 1024 → rate-limit reset → seq# → risk checks → match → post-trade → broadcast | 50 µs (only when queue empty) |
| **Stats thread** | Print 10-second snapshot to stdout | 10 s |
| **Main thread** | uWS event loop (`ws_server.run()`) — handles all WebSocket + REST I/O | blocks |
| **User threads** | 10,000 threads (one per simulated user) pushing orders at profile-specific rates | Poisson-sampled sleep |

> **Single-threaded engine guarantee**: The order book, all accounts, and post-trade state are only ever written by the engine thread. No locks needed on the hot path.

---

## 4. Order Lifecycle (Full Flow)

```
[Client / UserSimulator]
         │
         │  JSON-RPC place_order  (WS)
         ▼
  ConcurrentQueue<Order>   ← lock-free MPMC
         │
         ▼
  [Engine Thread]
  │
  ├─ 1. Cancel sentinel check  (status=CANCELLED, qty=0 → route to cancel)
  │
  ├─ 2. Rate-limit increment  (messages_this_sec++)
  │
  ├─ 3. Sequencer.next()  → assigns monotonic seq#
  │
  ├─ 4. RiskEngine.check_all()  → 8 checks (fail-fast)
  │         If fail → record_rejection, continue
  │
  ├─ 5. MatchingEngine.process()
  │         ├─ Record arrival_best price (Layer 2 anchor)
  │         ├─ FOK pre-scan (available_qty ≥ order qty?)
  │         ├─ POST_ONLY: would_cross? → reject
  │         └─ Matching loop:
  │               ├─ STP check (same user → apply STP mode)
  │               ├─ Layer 2 impact band check per fill
  │               ├─ Reduce-only qty cap
  │               └─ Emit Trade, update remaining_qty
  │         └─ Residual handling:
  │               GTC → add_order (rests in book)
  │               IOC/FOK/MARKET → cancel residual
  │
  ├─ 6. Post-trade (per Trade):
  │         ├─ apply_trade(buyer)   → position update + fee deduction
  │         ├─ apply_trade(seller)  → position update + fee deduction
  │         ├─ vault.credit_fee()
  │         └─ broadcaster.publish("trade.*")
  │
  ├─ 7. Broadcast depth snapshot if book changed
  │
  └─ 8. Zero-sum check every 1000 orders
```

---

## 5. Pre-Trade Risk Checks

All 8 checks run **in order, fail-fast** before any order touches the book.

| # | Check | Condition to PASS |
|---|-------|-------------------|
| 1 | **Instrument state** | `state == TRADING` or `COOLDOWN` (COOLDOWN allows LIMIT GTC only — see Check 8) |
| 2 | **Price band** (LIMIT only) | BUY: `price ≤ mark × 1.05` · SELL: `price ≥ mark × 0.95` |
| 3 | **Size limits** | `min_lot ≤ qty ≤ max_lot` |
| 4 | **Rate limit** | `messages_this_sec < rate_limit` |
| 5 | **Position limit** | `abs(current_net + order_delta) ≤ max_position_size` |
| 6 | **Margin** | `available_balance ≥ additional_im_for_order` |
| 7 | **Reduce-only direction** | If `reduce_only=true`: SELL must have a LONG position, BUY must have a SHORT position |
| 8 | **COOLDOWN mode** | During COOLDOWN: MARKET, IOC, FOK orders are blocked; LIMIT GTC allowed |

**Rate limits by user type:**

| Type | Orders/sec |
|------|-----------|
| RETAIL | 10 |
| ALGO | 50 |
| MARKET_MAKER | 300 |

Rate counters reset every second by the engine thread.

---

## 6. Matching Engine (FIFO CLOB)

### Data structure

```
bids_: std::map<double, std::deque<Order>, std::greater<double>>
       └─ highest price first; begin() = best bid
asks_: std::map<double, std::deque<Order>>
       └─ lowest price first; begin() = best ask
```

Each price level is a `std::deque<Order>` — FIFO guaranteed within a price level.

### Matching rules

1. **Price-time priority**: highest bid matches lowest ask; within a price level, earliest order fills first.
2. **LIMIT**: fills if `buy_price ≥ ask_price` (or `sell_price ≤ bid_price`).
3. **MARKET**: fills at any price (ignores price check), IOC TIF.
4. **POST_ONLY**: rejected (not cancelled) if it would immediately cross.
5. **FOK**: pre-scans `available_qty(side, price)` before attempting; cancelled (no partial fill) if depth is insufficient.
6. **IOC**: fills as much as possible, cancels residual.
7. **GTC**: unfilled residual rests in the book; `open_order_margin` reserved.

### Layer 2 — Impact Band

After recording `arrival_best` (best opposite price when the order arrives), each fill is checked:

```
BUY:  fill_price ≤ arrival_best × (1 + impact_band_pct)   [2% default]
SELL: fill_price ≥ arrival_best × (1 - impact_band_pct)
```

If a fill would violate the impact band, matching stops. Residual follows TIF rules.

### Self-Trade Prevention (STP)

Evaluated at match time when `incoming.user_id == resting.user_id`:

| Mode | Action |
|------|--------|
| `CANCEL_INCOMING` (default) | Incoming order cancelled, resting stays |
| `CANCEL_RESTING` | Resting order cancelled, incoming continues matching |
| `CANCEL_BOTH` | Both orders cancelled |

### Reduce-Only at fill time

If `reduce_only=true`, `max_fill = min(remaining_qty, current_position_size)`. If the position is already closed (size=0), matching stops immediately.

---

## 7. Post-Trade Processing

Called once per Trade, twice (once for buyer, once for seller).

### Position Cases

| Case | Condition | Action |
|------|-----------|--------|
| A — Open | No existing position | Create new position at fill price |
| B — Increase | Existing position, same direction | VWAP entry price update |
| C — Partial reduce | Opposite direction, fill qty < position size | Realize PnL, reduce size |
| D — Close + flip | Opposite direction, fill qty ≥ position size | Realize full close PnL, open new position for excess |

### PnL realization

```
LONG realized PnL  = (fill_price − entry_price) × fill_qty
SHORT realized PnL = (entry_price − fill_price) × fill_qty
```

Realized PnL is credited/debited to `wallet_balance` immediately.

### Fees

```
taker fee = fill_qty × fill_price × 0.0005  (0.05%)
maker fee = fill_qty × fill_price × 0.0002  (0.02%)
```

50% of total fee revenue flows to the insurance vault (`vault.credit_fee`).

---

## 8. Margin System

### IMF / MMF formula (sqrt-based)

```
IMF(notional) = max(base_imf,  imf_factor  × √notional)
MMF(notional) = max(base_mmf,  mmf_factor  × √notional)

IM = IMF × notional
MM = MMF × notional
```

Default instrument parameters:

| Parameter | Value |
|-----------|-------|
| `base_imf` | 0.02 (2%) |
| `imf_factor` | 0.00003 |
| `base_mmf` | 0.01 (1%) |
| `mmf_factor` | 0.000015 |

**Example — 1 BTC at $50,000:**

```
notional = 50,000
IMF = max(0.02, 0.00003 × √50,000) = max(0.02, 0.0212) = 0.0212
IM  = 0.0212 × 50,000 = $1,060
MM  = max(0.01, 0.000015 × √50,000) = $530
```

### Key margin functions

| Function | Description |
|----------|-------------|
| `cross_equity(user, mark)` | `wallet_balance + Σ unrealized_pnl` |
| `available_balance(user, mark, inst)` | `equity − total_IM − open_order_margin` |
| `additional_im_for_order(o, user, inst, mark)` | Extra IM needed for a new order (0 if reducing) |
| `liq_price_estimate(pos, equity, inst)` | Approximate price where MM = equity |
| `bankruptcy_price(pos, allocated_margin)` | Price where equity = 0 |

### Liquidation threshold

A position is eligible for liquidation when:

```
cross_equity(user, mark) < total_cross_MM(user, mark, inst)
```

---

## 9. Mark Price & GBM Simulation

### Mark Price Engine

```
EWMA_basis = α × (mid_price − index_price) + (1−α) × EWMA_basis
mark = clamp(index_price + EWMA_basis, index_price × 0.95, index_price × 1.05)
```

- `α = 0.0116` → 1-minute half-life with 100ms ticks
- Hard clamp: mark price can never deviate more than ±5% from index price
- `basis()` returns the current EWMA basis

### GBM Price Simulator

```
dS = μ·S·dt + σ·S·dW
S_{t+1} = S_t × exp((μ − σ²/2)·dt + σ·√dt·Z)    Z ~ N(0,1)
```

| Parameter | Value | Description |
|-----------|-------|-------------|
| `S₀` | 50,000 | Starting price |
| `μ` | 0.0 | No drift (stress-test mode) |
| `σ` | 0.60 | 60% annual vol |
| `dt` | 0.1 s | 100ms tick interval |
| Floor | 1.0 | Price cannot go below $1 |

---

## 10. Stop Orders

Stop-limit orders sit in the `StopTriggerQueue`. The mark price thread scans every 100ms.

### Trigger sources

| Source | Fires when |
|--------|-----------|
| `MARK` | mark_price crosses trigger_price |
| `LAST_TRADED` | last trade price crosses trigger_price |
| `INDEX` | index_price crosses trigger_price |

### COOLDOWN freeze

Stop triggers are **frozen** while `inst.state == COOLDOWN`. The scanner skips all triggers in cooldown mode to prevent cascading stop liquidations during volatile periods.

### Margin reservation

- Normal stop: IM reserved at placement time (locks capital).
- `reduce_only=true` stop: **no IM reserved** (it only reduces an existing position).

---

## 11. Liquidation Engine

Triggered when `cross_equity < total_MM`. Runs entirely on the engine thread (synchronously, during risk monitor callback).

### Liquidation waterfall

```
Step 1: Cancel all open orders
        → releases open_order_margin
        → re-check: if equity > MM now, abort (margin freed was enough)

Step 2: Select worst position (largest unrealized loss)

Step 3: Gradual loop — repeat until equity > MM or position closed:
        chunk_size = max(min_lot, position_size × 10%)

        Build a reduce-only IOC liquidation order:
          LONG  → SELL at mark × 0.95  (5% below mark)
          SHORT → BUY  at mark × 1.05  (5% above mark)

        Process through matching engine (bypasses pre-trade risk)

        For each fill:
          Deduct liq_fee = qty × price × 1%  → credit vault
          Check bankruptcy:
            LONG bankrupt price  = entry_price − margin/size
            SHORT bankrupt price = entry_price + margin/size
          If fill_price worse than bankruptcy_price:
            deficit = |fill_price − bankruptcy_price| × qty
            vault.absorb_deficit(deficit)  → if vault empty → ADL

Step 4: Mark position as CLOSED if size < epsilon
```

### Liquidation fee

1% of notional, deducted from the liquidated user's wallet, credited to the insurance vault.

---

## 12. Circuit Breakers

Four layers of market protection:

| Layer | Where enforced | Description |
|-------|---------------|-------------|
| **1 — Price Band** | `risk_checks.cpp` | Rejects LIMIT orders >5% from mark price |
| **2 — Impact Band** | `matching.cpp` | Stops matching loop if fill deviates >2% from arrival best |
| **3 — Velocity** | `circuit_breakers.cpp` | 8% price move in 60s → 30s COOLDOWN |
| **4 — Market-wide** | `circuit_breakers.cpp` | All instruments in COOLDOWN OR vault >90% utilised → EMERGENCY |

### COOLDOWN behaviour

During `COOLDOWN`:
- MARKET, IOC, FOK orders → rejected
- LIMIT GTC orders → allowed (liquidity provision)
- Stop triggers → frozen
- After 30s → automatically returns to TRADING

### EMERGENCY behaviour

- All instruments set to EMERGENCY state
- No orders accepted
- Requires **manual admin intervention** to recover (no auto-resume)

---

## 13. Insurance Vault & ADL

### Insurance Vault

- Initial seed: **$200,000**
- SITG (skin-in-the-game): **$100,000** permanently locked exchange capital
- Funded by: 50% of all trading fees + liquidation fees
- `utilization = total_losses_absorbed / (current_balance + total_losses_absorbed)`
- If vault utilization > 90% → triggers Layer 4 circuit breaker

### Auto-Deleveraging (ADL)

When the vault cannot cover a deficit:

1. Rank all users with opposing positions by profit (most profitable first)
2. ADL the most profitable counterparty at the bankruptcy price
3. The counterparty's position is forcibly reduced — no fee, no choice
4. Broadcasts `liquidation.<instrument>` event with `"type": "ADL"`

---

## 14. WebSocket API

**Endpoint:** `ws://localhost:9001/`

All messages are JSON-RPC style objects.

### Connection flow

```
connect → ws://localhost:9001/
  → send auth message
  → send SUBSCRIBE message
  → receive stream data
```

### Authentication

```json
{
  "method": "auth",
  "user_id": "U1234"
}
```

For simulation, user IDs are `U0` through `U9999`. In production this would verify a JWT/HMAC signature.

### Subscribe

```json
{
  "method": "SUBSCRIBE",
  "params": [
    "trade.BTC-USDC-PERP",
    "depth.BTC-USDC-PERP",
    "markPrice.BTC-USDC-PERP"
  ]
}
```

Private streams require authentication first and are namespaced by user:

```json
{
  "method": "SUBSCRIBE",
  "params": ["account.orders", "account.positions", "account.warnings"]
}
```

### Unsubscribe

```json
{
  "method": "UNSUBSCRIBE",
  "params": ["depth.BTC-USDC-PERP"]
}
```

### Place Order

```json
{
  "method": "place_order",
  "instrument": "BTC-USDC-PERP",
  "side": "BUY",
  "type": "LIMIT",
  "price": 50000.0,
  "quantity": 0.1,
  "tif": "GTC",
  "reduce_only": false,
  "stp_mode": "CANCEL_INCOMING",
  "margin_mode": "CROSS"
}
```

| Field | Values | Default |
|-------|--------|---------|
| `side` | `BUY`, `SELL` | required |
| `type` | `LIMIT`, `MARKET`, `STOP_LIMIT`, `POST_ONLY` | `LIMIT` |
| `tif` | `GTC`, `IOC`, `FOK` | `GTC` |
| `stp_mode` | `CANCEL_INCOMING`, `CANCEL_RESTING`, `CANCEL_BOTH` | `CANCEL_INCOMING` |
| `margin_mode` | `CROSS`, `ISOLATED` | `CROSS` |
| `reduce_only` | `true`, `false` | `false` |
| `price` | decimal | required for LIMIT |
| `quantity` | decimal (≥ 0.001, ≤ 500) | required |

### Cancel Order

```json
{
  "method": "cancel_order",
  "order_id": "WS-0000000001"
}
```

---

## 15. REST API

Base URL: `http://localhost:9001`

### GET /api/v1/depth

Returns L2 order book snapshot.

Query params:
- `depth` (optional, default 100) — number of price levels per side

```bash
curl http://localhost:9001/api/v1/depth?depth=5
```

Response:
```json
{
  "instrument": "BTC-USDC-PERP",
  "seq": 123456,
  "timestamp": 1711234567890123,
  "bids": [
    [49950.0, 2.5],
    [49900.0, 1.2]
  ],
  "asks": [
    [50050.0, 0.8],
    [50100.0, 3.1]
  ]
}
```

### GET /api/v1/ticker

Returns current market snapshot.

```bash
curl http://localhost:9001/api/v1/ticker
```

Response:
```json
{
  "instrument": "BTC-USDC-PERP",
  "mark_price": 50023.45,
  "index_price": 50018.90,
  "best_bid": 49998.0,
  "best_ask": 50001.0,
  "spread": 3.0,
  "orders": 4821,
  "trades": 18203
}
```

### GET /api/v1/account

Returns account state for a user (simulation — no auth required via REST).

```bash
curl "http://localhost:9001/api/v1/account?user_id=U0"
```

Response:
```json
{
  "user_id": "U0",
  "wallet_balance": 98234.50,
  "open_order_margin": 1500.00,
  "unrealized_pnl": 230.45,
  "equity": 98464.95,
  "positions": [
    {
      "instrument": "BTC-USDC-PERP",
      "side": "LONG",
      "size": 0.5,
      "entry_price": 49800.0,
      "unrealized_pnl": 230.45
    }
  ]
}
```

---

## 16. Public Streams Reference

All public streams are available without authentication.

| Topic | Trigger | Payload |
|-------|---------|---------|
| `trade.<instrument>` | Every fill | `{trade_id, price, quantity, buyer_id, seller_id, timestamp}` |
| `depth.<instrument>` | Every book change | Full L2 snapshot up to 20 levels |
| `markPrice.<instrument>` | Every 100ms | `{mark_price, index_price, basis}` |
| `liquidation.<instrument>` | Liquidation/ADL event | `{type, user_id, fill_qty, fill_price}` |

Stream format (envelope):

```json
{
  "stream": "trade.BTC-USDC-PERP",
  "data": { ... }
}
```

---

## 17. Private Streams Reference

Require `auth` before subscribing. Internally stored as `channel.<user_id>`.

| Topic | Trigger | Payload |
|-------|---------|---------|
| `account.orders` | Order status change | Order object |
| `account.positions` | After each fill | Full account JSON (wallet, positions) |
| `account.warnings` | Margin warning or liquidation | `{type, equity, total_im}` or `{type: "LIQUIDATION", instrument}` |

---

## 18. WebSocket Message Examples

### Quick-start with `websocat`

```bash
# Install: brew install websocat
websocat ws://localhost:9001/
```

### Subscribe to trades and mark price

```json
{"method":"SUBSCRIBE","params":["trade.BTC-USDC-PERP","markPrice.BTC-USDC-PERP"]}
```

### Authenticate and place a limit order

```json
{"method":"auth","user_id":"U0"}
{"method":"place_order","instrument":"BTC-USDC-PERP","side":"BUY","type":"LIMIT","price":49000,"quantity":0.1,"tif":"GTC"}
```

### Place a market order

```json
{"method":"auth","user_id":"U0"}
{"method":"place_order","instrument":"BTC-USDC-PERP","side":"SELL","type":"MARKET","quantity":0.05,"tif":"IOC"}
```

### Place a FOK order

```json
{"method":"place_order","instrument":"BTC-USDC-PERP","side":"BUY","type":"LIMIT","price":50500,"quantity":1.0,"tif":"FOK"}
```

### Cancel an order

```json
{"method":"cancel_order","order_id":"WS-0000000001"}
```

### Subscribe to private account stream

```json
{"method":"auth","user_id":"U5"}
{"method":"SUBSCRIBE","params":["account.positions","account.warnings"]}
```

### Sample trade event received

```json
{
  "stream": "trade.BTC-USDC-PERP",
  "data": {
    "trade_id": "T-000001",
    "price": 50023.5,
    "quantity": 0.1,
    "buyer_id": "U0",
    "seller_id": "U4321",
    "timestamp": 1711234567890123
  }
}
```

### Sample margin warning received

```json
{
  "stream": "account.warnings.U0",
  "data": {
    "type": "MARGIN_WARNING",
    "equity": 1230.45,
    "total_im": 1100.00
  }
}
```

---

## 19. Simulation Layer

### User profiles

| Profile | Count | Order rate | Order mix | Price offset |
|---------|-------|-----------|-----------|-------------|
| Retail | 8,000 | Poisson λ=0.05/s | 70% limit, 25% market, 5% stop | ±0–2% of mark |
| Algo | 1,500 | Poisson λ=0.5/s | 50% limit, 40% market, 10% stop | ±0–0.5% |
| Market Maker | 500 | Poisson λ=5/s (both sides) | 99% limit pairs | ±spread/2 (0.01–0.05%) |

### Account setup

- Each user gets a log-normally distributed balance: $1,000–$100,000
- `rate_limit` assigned by profile (retail=10, algo=50, mm=300)
- All start with empty positions

### Throughput target

The engine thread processes up to **1,024 orders per 50µs loop**. With 10,000 users at these rates, expected steady-state is **5,000–15,000 orders/sec** depending on hardware.

---

## 20. Statistics & Zero-Sum Check

### Terminal output (every 10 seconds)

```
=== Asgard Stats [T+10s] ===
Orders/sec:    8,234
Trades/sec:    1,891
Fill rate:     22.97%
Total orders:  82,340
Total trades:  19,288
Rejections:    5,102
Liquidations:  3
Vault:         $201,234.50  util=0.3%
Best bid/ask:  49,998.0 / 50,001.0  spread=$3.00
Mark price:    50,000.2
Zero-sum err:  $0.02
```

### Zero-sum invariant

Every 1,000 orders, the engine verifies:

```
Σ(wallet_balance) + Σ(unrealized_pnl) + fee_revenue + vault_balance ≈ total_deposited
```

Tolerance: $1.00 (floating-point accumulation). A warning is printed if exceeded.

### Latency metrics

Latency is measured as `trade.timestamp_us − order.timestamp_us` (microseconds).

| Percentile | Expected (localhost simulation) |
|-----------|--------------------------------|
| p50 | ~50 µs |
| p95 | ~200 µs |
| p99 | ~500 µs |

---

## 21. Test Suite

40 test cases, 76 assertions across 4 files.

### test_orderbook.cpp

| Test | What it verifies |
|------|-----------------|
| Best bid/ask after adding orders | `best_bid()=101`, `best_ask()=102`, `mid_price()=101.5` |
| FIFO within price level | Alice before Bob before Carol at same price |
| remove_order by ID | Removes correctly; returns false for unknown ID |
| available_qty for FOK | Sweeps up to price limit; returns cumulative qty |
| would_cross for POST_ONLY | `true` at matching price, `false` below |
| Price level cleanup | `consume_front` auto-prunes empty levels |

### test_matching.cpp

| Test | What it verifies |
|------|-----------------|
| Simple bid-ask fill | 1 trade, correct price/qty/buyer/seller |
| FIFO fill order | Two resting sells → first-in fills first |
| FOK cancelled insufficient depth | No partial fill; resting order preserved |
| IOC partial fill | Fills available qty; residual cancelled; book empty after |
| POST_ONLY rejected on cross | Zero trades; resting sell preserved |
| STP CANCEL_INCOMING | Self-trade prevented; resting preserved |
| Cancel resting order | `cancel()` returns true; book empty |
| Partial fill leaves residual | 2.0 resting → 1.0 filled → 1.0 remains |

### test_risk.cpp

| Test | What it verifies |
|------|-----------------|
| PRE_OPEN rejects | `check_instrument_state` fails for non-TRADING |
| TRADING passes | State check passes |
| BUY above band rejected | 6% above mark → fail |
| SELL below band rejected | 6% below mark → fail |
| Within band passes | 4% above mark → pass |
| Below min_lot rejected | qty=0.0005 < 0.001 → fail |
| Above max_lot rejected | qty=200 > 100 → fail |
| Valid lot size passes | qty=1.0 → pass |
| Rate limit exceeded | `messages_this_sec == rate_limit` → fail |
| Rate limit not exceeded | `messages_this_sec == rate_limit - 1` → pass |
| Position limit exceeded | 90+20=110 > 100 → fail |
| Reduce-only no position | No existing position → fail |
| Reduce-only wrong direction | SELL on SHORT position → fail |
| Reduce-only valid | SELL 1.0 on LONG 2.0 → pass |
| MARKET blocked in COOLDOWN | `check_cooldown_mode` fails |
| LIMIT GTC in COOLDOWN | Passes |

### test_margin.cpp

| Test | What it verifies |
|------|-----------------|
| IMF base floor | Small notional → returns `base_imf` |
| IMF sqrt scaling | Large notional → returns `imf_factor × √notional` |
| MMF calculation | Correct MMF at given notional |
| cross_equity | `wallet + unrealized_pnl` |
| available_balance | `equity − IM − open_order_margin` |
| Bankruptcy price long | `entry − margin/size` |
| Bankruptcy price short | `entry + margin/size` |
| additional_im new order | Positive for position-increasing order |
| additional_im reduce-only | Returns 0 |

---

## Instrument Configuration Reference

```cpp
BTC-USDC-PERP defaults:
  min_lot            = 0.001 BTC
  max_lot            = 500.0 BTC
  lot_step           = 0.001 BTC
  price_band_pct     = 5%    (Layer 1)
  impact_band_pct    = 2%    (Layer 2)
  velocity_threshold = 8%    (Layer 3 trigger)
  velocity_window_s  = 60    seconds
  cooldown_duration_s = 30   seconds
  base_imf           = 2%
  imf_factor         = 0.00003
  base_mmf           = 1%
  mmf_factor         = 0.000015
  max_position_size  = 500.0 BTC
  maker_fee_rate     = 0.02%
  taker_fee_rate     = 0.05%
  liq_fee_rate       = 1%
  liq_chunk_pct      = 10%   (per liquidation iteration)
  liq_band_pct       = 5%    (liquidation order price offset from mark)
```
