V1 tech 

# Asgard FnO — System Design

## Flow 2: Order Placement → Book

### Overview

An order enters the system. This is the hot path — the most latency-sensitive flow in the entire exchange. Every microsecond between order submission and acknowledgment matters, especially for market makers.

The pipeline: **API Gateway → Sequencer → Pre-Trade Risk → Matching Engine → Book (or Trade)**

### How TradFi handles this

The Asgard order pipeline is structurally identical to CME Globex and NSE:

TradFi (CME):

  FIX Gateway → Market Segment Gateway (FIFO) → Pre-trade Risk → Matching Engine → Book

Asgard:

  WebSocket API → Sequencer (FIFO) → Pre-trade Risk → Matching Engine → Book

CME's Market Segment Gateways enforce FIFO ordering — the same role our Sequencer plays. NSE has an Accumulator that takes orders from multiple gateways and sequences them into a single ordered stream. Same concept, same purpose: **fairness through deterministic ordering.**

CME processes \~20 million orders/day. NSE measures latency in microseconds. We're targeting 10,000 orders/sec — well within what a single-threaded sequencer can handle.

### Order Types

Different traders have different needs. A retail trader wants to buy at the current price. A market maker wants to passively rest in the book and earn the spread. Each needs a different instruction to the matching engine.

**By price instruction:**

| Type | What it means | Who needs it |
| :---- | :---- | :---- |
| **Limit** | Buy/sell at this price or better | Everyone. The fundamental order type. |
| **Market** | Buy/sell immediately at whatever price is available | Retail, liquidation engine. Fast execution, no price guarantee. |
| **Stop-Limit** | When mark price hits trigger, place a limit order. A sleeping order. | Traders managing risk (stop-loss, take-profit). |
| **Post-Only** | Place limit order, but reject if it would immediately match. Guarantees maker status. | Market makers. They never want to accidentally cross the spread and pay taker fees. |

**By modifier flags:**

| Flag | What it means | Who needs it |
| :---- | :---- | :---- |
| **Reduce-Only** | Order can only reduce or close an existing position. Rejected if it would open or increase a position. | Everyone closing positions in volatile markets. Prevents accidentally opening a new position if the order overfills or the position was already closed by another order. Adopted from Backpack — they include this as a standard flag. |

**By time-in-force (how long the order lives):**

| Type | What it means | Who needs it |
| :---- | :---- | :---- |
| **GTC** (Good-til-cancelled) | Stays in book until filled or manually cancelled. Default. | Everyone. Standard for limit orders. |
| **IOC** (Immediate-or-cancel) | Fill whatever you can right now, cancel the rest. | Algos, liquidation engine. Want execution, not resting orders. |
| **FOK** (Fill-or-kill) | Fill entire quantity right now, or cancel everything. All-or-nothing. | Large orders that need certainty of full fill. |

**For market makers:**

| Feature | What it does | Why |
| :---- | :---- | :---- |
| **mass\_quote** | Update bid \+ ask for an instrument in a single message | MM requotes constantly. One message instead of cancel+cancel+place+place \= 4x less latency, 4x less rate limit consumption. CME supports equivalent "mass quote" functionality. |
| **Cancel-on-disconnect** | Auto-cancel all MM's orders if their connection drops | Stale quotes \= free money for arbitrageurs. MM can't manage quotes if disconnected. Industry standard at every exchange. |

**Self-trade prevention (STP):**

When a user (usually an MM) has both buy and sell orders, a new order might match against their own resting order. This is economically pointless — you'd pay fees to trade with yourself.

| STP Mode | What happens |
| :---- | :---- |
| Cancel incoming | New order rejected, resting order stays |
| Cancel resting | Resting order cancelled, new order enters book |
| Cancel both | Both cancelled |

User selects their STP mode. Default: cancel incoming. CME and most TradFi exchanges have equivalent self-trade prevention.

**Not included for MVP:**

- Iceberg/hidden orders — adds matching engine complexity, only matters at high volume  
- Trailing stop — nice to have, not critical  
- TWAP orders — splits large orders over time, good for V1.1 (Backpack has this)  
- Scaled orders — multiple orders at price intervals for DCA/ladder strategies, V1.1

### The Sequencer

The most important component for system integrity.

**Why it exists:** Orders arrive from hundreds of users over WebSocket simultaneously. Two buy orders at the same price might arrive within microseconds. In a FIFO matching engine (price-time priority), who was first determines who gets filled first. Real money at stake.

The sequencer guarantees: **every message gets a unique, monotonically increasing sequence number, and all messages are processed in that order.**

Every inbound message → sequence number → processed in order

Messages that get sequenced:

  \- New order

  \- Cancel order

  \- Modify order

  \- mass\_quote (MM batch update)

  \- Admin commands (halt instrument, etc.)

**Single-writer by design.** One thread assigns sequence numbers. This is an intentional bottleneck — it guarantees total ordering. The matching engine processes messages strictly in sequence order. No ambiguity, no race conditions.

**The sequence log is the write-ahead log (WAL):**

seq=1  | NEW\_ORDER   | user=alice | buy  | BTC-USDC-20260327 | limit 50000 | qty 2 | GTC

seq=2  | NEW\_ORDER   | user=bob   | sell | BTC-USDC-20260327 | limit 50100 | qty 1 | GTC

seq=3  | CANCEL      | user=alice | order\_id=0x001

seq=4  | NEW\_ORDER   | user=carol | sell | BTC-USDC-20260327 | limit 50000 | qty 1 | IOC

seq=5  | MASS\_QUOTE  | user=mm1   | BTC-USDC-20260327 | bid 49990/10 | ask 50010/10

...

If the matching engine crashes, we replay the WAL from the last checkpoint to rebuild exact state. Every order, every cancel, every trade — deterministically reproducible from the log. This is exactly how CME Globex and every serious exchange works.

**How TradFi compares:**

- CME: Market Segment Gateways enforce FIFO. The gateway itself is the sequencer.  
- NSE: Accumulator takes orders from gateways and sequences into a single ordered stream.  
- Eurex: Enhanced Trading Interface with deterministic message ordering.

### Pre-Trade Risk Checks

After sequencing, before the order touches the book. Every order passes through these checks in order. If any check fails, the order is rejected immediately.

**Check 1 — Instrument validation**

if instrument.state \!= TRADING:

    reject("Instrument not available for trading")

The instrument might be expired, in settlement, halted by circuit breaker, or not yet listed. Only `TRADING` state accepts orders.

**Check 2 — Price validation (fat finger protection)**

Prevents catastrophic typos. A user accidentally placing a buy at $500,000 instead of $50,000 would sweep the entire ask side of the book.

For buy orders:  price \<= mark\_price × (1 \+ price\_band%)

For sell orders: price \>= mark\_price × (1 \- price\_band%)

Example: mark \= 50,000, band \= 5%

  Buy limit at 53,000  → REJECTED (above 52,500 ceiling)

  Buy limit at 51,000  → OK

  Sell limit at 46,000  → REJECTED (below 47,500 floor)

Market orders skip this — they fill at whatever's available. Circuit breakers (Flow 8\) protect against extreme fills.

CME calls this "price banding" and applies dynamic bands around the reference price. NSE has similar price filters.

**Check 3 — Size validation**

if quantity \< instrument.min\_lot\_size:

    reject("Below minimum lot size")

if quantity \> instrument.max\_order\_size:

    reject("Exceeds maximum order size")

if quantity % instrument.lot\_step \!= 0:

    reject("Quantity must be in whole lots")

**Check 4 — Rate limiting**

if user.messages\_this\_second \> user.rate\_limit:

    reject("Rate limit exceeded")

Market makers get higher rate limits (they need to requote constantly — a 200ms quote update cycle means 5 messages/sec just for one instrument). Retail gets lower limits.

| User type | Rate limit (indicative) |
| :---- | :---- |
| Retail | 10 messages/sec |
| API trader | 50 messages/sec |
| Market maker | 300 messages/sec |

These are per-user, not per-connection. CME has similar tiered rate limits.

**Check 5 — Position limit**

hypothetical\_position \= current\_position \+ (order\_side × order\_quantity)

if abs(hypothetical\_position) \> instrument.max\_position\_size:

    reject("Would exceed position limit")

Prevents any single user from accumulating a dangerously large position. CME and CFTC enforce "speculative position limits." NSE/SEBI enforce market-wide and client-level position limits.

**Check 6 — Margin check**

The most complex check. Two critical insights:

**Insight 1:** We must reserve margin for resting (open) orders, not just existing positions. If a user places 10 limit buy orders at different prices and none fill yet, a sudden price crash could fill all 10 simultaneously. Without reserved margin, the user would be undercollateralized the instant they fill.

**Insight 2:** Orders that reduce a position don't need additional margin — they free margin.

┌──────────────────────────────────────────┐

│          User's Margin State             │

│                                          │

│  equity \= wallet\_balance \+ unrealized\_pnl│

│                                          │

│  reserved \= position\_margin              │  ← IM for existing positions

│           \+ open\_order\_margin            │  ← IM reserved for resting orders

│                                          │

│  available \= equity \- reserved           │

│                                          │

└──────────────────────────────────────────┘

New order arrives:

  INCREASES position (or opens new):

    additional\_IM \= IM for the order's size

    if available \< additional\_IM → REJECT

  REDUCES position (selling a long, buying back a short):

    additional\_IM \= 0

    → PASS (no new margin needed — position shrinks)

  FLIPS position (short 5, buy 8 → net long 3):

    additional\_IM \= IM for the net new direction (long 3\)

    minus margin freed from closing the short (5)

    → may be positive or negative

**Cross-margin mode:**

available \= equity \- Σ(IM for all cross positions) \- Σ(IM for all cross resting orders)

**Isolated-margin mode:** User specifies the margin amount for this position.

if specified\_margin \< instrument.minimum\_IM:

    reject("Below minimum initial margin")

if specified\_margin \> free\_balance:

    reject("Insufficient balance")

**Check 7 — Self-trade prevention**

Note: STP is listed here as a pre-trade check conceptually, but in implementation it is evaluated inside the matching engine at match time — the engine checks whether the incoming order would cross a resting order from the same user. It cannot be a pure pre-trade gate because you only know the match counterparty when you walk the book.

During matching, if incoming order would match against same user's resting order:

    apply STP mode:

      CANCEL\_INCOMING: reject new order, resting order stays

      CANCEL\_RESTING:  cancel resting order, new order enters book

      CANCEL\_BOTH:     cancel both

### The Full Flow

User (via WebSocket)              Off-chain Engine

────────────────────              ────────────────

1\. User sends order message:

   {

     type: "limit",

     side: "buy",

     instrument: "BTC-USDC-20260327",

     price: 50000,

     quantity: 2,

     time\_in\_force: "GTC",

     margin\_mode: "cross"

   }

        │

        ▼

2\. API Gateway:

   \- Authenticate (valid session, valid user)

   \- Parse message format

   \- Basic field validation (required fields present, correct types)

   \- Forward to sequencer

        │

        ▼

3\. Sequencer:

   \- Assign sequence number (seq=1274)

   \- Write to WAL (persistent, append-only)

   \- Forward to processing pipeline in sequence order

        │

        ▼

4\. Pre-Trade Risk Engine:

   \- Instrument valid & in TRADING state?     ✓

   \- Price within bands?                      ✓ (50000 within 5% of mark)

   \- Size within limits?                      ✓ (2 lots, above min, below max)

   \- Rate limit OK?                           ✓ (user under limit)

   \- Position limit OK?                       ✓ (won't exceed max position)

   \- Margin sufficient?                       ✓ (available ≥ IM for 2 lots)

   \- Self-trade check?                        ✓ (no conflict with resting orders)

        │

        ▼

5\. Matching Engine receives the order.

   Checks the sell side of BTC-USDC-20260327 book:

   SELL SIDE (asks):

   ┌────────────────────────────┐

   │ 50200  |  3 lots  (bob)    │

   │ 50100  |  1 lot   (carol)  │

   │        |  — no asks at     │

   │        |    50000 or below —│

   └────────────────────────────┘

   Incoming buy limit at 50000\. Best ask is 50100\.

   50000 \< 50100 → no match possible.

        │

        ▼

6\. Order rests in the book at 50000:

   BUY SIDE (bids):                  SELL SIDE (asks):

   ┌──────────────────────────┐      ┌────────────────────────────┐

   │ 50000 | 2 lots (alice) ★ │      │ 50100 | 1 lot   (carol)   │

   │ 49900 | 5 lots (dave)    │      │ 50200 | 3 lots  (bob)     │

   └──────────────────────────┘      └────────────────────────────┘

   ★ \= newly placed

        │

        ▼

7\. Engine updates internal state:

   \- Order state: NEW → OPEN (resting in book)

   \- alice's open\_order\_margin \+= IM for 2 lots

   \- alice's available\_balance \-= IM for 2 lots

        │

        ▼

8\. Broadcast (parallel):

   a) WebSocket to alice:

      { order\_id: "0xabc", status: "open", price: 50000, qty: 2, seq: 1274 }

   b) Market data (all subscribers):

      { type: "book\_update", bid: \[\[50000, 2\], \[49900, 5\]\], ask: \[\[50100, 1\], \[50200, 3\]\] }

   c) Audit trail:

      { seq: 1274, event: "order\_placed", user: alice, details: {...}, timestamp: ... }

**If the order WOULD have matched** (alice's buy at 50100 or higher):

5\. Matching Engine:

   Buy limit at 50100\. Best ask is 50100 (carol, 1 lot).

   50100 \>= 50100 → MATCH at 50100 for 1 lot.

   Trade: alice buys 1 lot from carol at 50100 ──► Flow 3 picks up here

                                                    (position creation, margin

                                                     recalculation, market data)

   Remaining: alice still wants 1 more lot.

   Next ask: 50200 (bob). 50100 \< 50200 → no more matches.

   Remaining 1 lot rests in book at 50100\.

**Behavior by order type:**

| Order type | No immediate match | Partial match | Full match |
| :---- | :---- | :---- | :---- |
| **Limit GTC** | Rests in book | Fills partial, rest rests in book | Filled, done |
| **Limit IOC** | Cancelled entirely | Fills partial, rest cancelled | Filled, done |
| **Limit FOK** | Cancelled entirely | Cancelled entirely (all-or-nothing) | Filled, done |
| **Market** | Cancelled (empty book) | Fills at available prices, rest cancelled | Filled, done |
| **Post-Only** | Rests in book | REJECTED (would have matched) | REJECTED (would have matched) |

### Stop-Limit Orders — The Trigger Queue

Stop orders don't go to the matching engine immediately. They sit in a separate trigger queue and activate when a price condition is met.

**Trigger price sources** (user selects, adopted from Backpack):

| Source | Behavior | When to use |
| :---- | :---- | :---- |
| **Mark price** (default) | Triggers on mark price movement. Filters temporary wicks/spikes. | Most reliable. Recommended default. |
| **Last traded price** | Triggers on actual trade executions. Most reactive. | When you want to react to real market activity. |
| **Index price** | Triggers on the oracle index. Most stable, hardest to manipulate. | When you want maximum manipulation resistance. |

User places: Stop-Limit Buy

  trigger\_price: 51000

  trigger\_source: mark  (or last, or index)

  limit\_price:   51100

  quantity:      1

               ┌───────────────────┐

               │   Trigger Queue    │

               │                    │

               │  Condition:        │

               │  mark\_price ≥ 51000│

               │                    │

               │  Watching...       │

               └────────┬──────────┘

                        │

  Mark price hits 51000 ┘

                        │

                        ▼

         Limit buy order at 51100, qty 1

         injected into normal pipeline:

         Sequencer → Risk → Matching Engine

The trigger queue is scanned every time the mark price updates. Mark price update frequency directly determines stop order responsiveness.

**Margin for stop orders:** IM is reserved when the stop order is placed, not when it triggers. Otherwise a user could place stops they can't afford and the margin check would fail at trigger time — potentially during a volatile moment when execution matters most.

**Exception: reduce-only stop orders do NOT reserve IM.** A reduce-only stop can only close or reduce an existing position — it frees margin rather than consuming it. Reserving IM for a reduce-only stop would be double-counting (the position already has margin locked). This matches how Backpack and Binance handle reduce-only stops.

### Cancel & Modify

**Cancel order:**

User sends: cancel order\_id=0xabc

  → Sequencer: assign seq number, write WAL

  → Engine: find order in book, remove it

  → Release reserved margin: open\_order\_margin \-= IM

  → available\_balance restored

  → WebSocket to user: { order\_id: "0xabc", status: "cancelled" }

  → Market data: book update (depth at that price level changed)

  → Audit trail: log cancellation

**Modify order (cancel-replace):**

User sends: modify order\_id=0xabc, new\_price=49900, new\_qty=3

  → Sequencer: assign seq number, write WAL

  → Engine: cancel old order first, then attempt to place new order

  → New order gets a NEW sequence number → loses time priority

  → If new order needs more margin:

      margin check runs → if fails, new order rejected

      WARNING: old order is already cancelled at this point.

      The user ends up with NO order. This is intentional —

      it matches CME and Eurex behavior for cancel-replace.

      Alternative (check margin before cancelling) adds complexity

      and creates a window where both old and new order exist.

  → If new order needs less margin:

      excess margin released

  → WebSocket: old order cancelled \+ new order acknowledged (or rejection)

  → Market data: book updated

**Losing time priority on modify is universal.** CME, NSE, Eurex — every exchange works this way. If you change your order, you go to the back of the queue at that price level. This prevents gaming (modifying to jump the queue without risk).

The one exception: CME allows quantity-decrease modifications that preserve priority (you're making your order smaller, which can't disadvantage anyone). Worth considering for V2.

### System Diagram — Order Placement

                         ┌──────────────────┐

                         │   User / MM       │

                         │  (WebSocket/API)  │

                         └────────┬─────────┘

                                  │

                                  ▼

                         ┌──────────────────┐

                         │   API Gateway     │  authenticate, parse,

                         │                   │  validate format

                         └────────┬─────────┘

                                  │

                                  ▼

                         ┌──────────────────┐

                         │   Sequencer       │  assign seq\#,

                         │                   │  write to WAL

                         └────────┬─────────┘

                                  │

                                  ▼

                         ┌──────────────────┐

                         │  Pre-Trade Risk   │  instrument, price band,

                         │  Engine           │  size, rate limit,

                         │                   │  position limit, margin,

                         │                   │  self-trade prevention

                         └────────┬─────────┘

                                  │

                           ┌──────┴──────┐

                           │             │

                        REJECT        PASS

                           │             │

                           ▼             ▼

                      WebSocket    ┌──────────────────┐

                      rejection    │  Matching Engine   │  check opposite side

                      to user      │  (FIFO price-time) │  of book for crosses

                                   └────────┬─────────┘

                                            │

                                     ┌──────┴──────┐

                                     │             │

                                  NO MATCH      MATCH

                                     │             │

                                     ▼             ▼

                               Order rests    Trade executes

                               in book        → Flow 3

                                     │

                                     ▼

                         ┌──────────────────────┐

                         │     Broadcast         │

                         │                       │

                         │ • Order ack → user    │

                         │ • Book update → all   │

                         │ • Audit log → store   │

                         └──────────────────────┘

  ┌──────────────────┐

  │  Trigger Queue    │  stop orders sit here,

  │  (mark price      │  activate when mark price

  │   driven)         │  crosses trigger

  └──────────────────┘

          │

          │ triggered → injected into Sequencer

          ▼            (normal pipeline from there)

---

### Decisions Made — Flow 2

| \# | Decision | Choice | Rationale |
| :---- | :---- | :---- | :---- |
| 1 | Order types (MVP) | Limit, Market, Stop-Limit, Post-Only \+ Reduce-Only flag | Covers retail, algos, MMs. Reduce-Only prevents accidental position opens during close (adopted from Backpack). |
| 2 | Time-in-force (MVP) | GTC, IOC, FOK | GTC default for limits, IOC for algos/liquidation, FOK for large fill certainty. |
| 3 | MM features | mass\_quote \+ cancel-on-disconnect | Essential for MM onboarding. mass\_quote \= 4x less latency. Cancel-on-disconnect \= safety. |
| 4 | Self-trade prevention | Yes, user-selectable mode (cancel incoming default) | Required for MMs who quote both sides. Industry standard. |
| 5 | Sequencer model | Single-writer, sequence log \= WAL | Deterministic ordering, crash recovery via replay. Same model as CME MSGWs and NSE Accumulator. |
| 6 | Margin reservation | Reserve IM for resting orders, not just positions | Prevents undercollateralization when multiple orders fill simultaneously. |
| 7 | Stop order margin | Reserve IM at placement, not at trigger | Ensures margin is available when stop triggers during volatility. |
| 8 | Modify \= cancel \+ new | New order gets new sequence number, loses time priority | Universal exchange behavior. Prevents queue-jumping. |
| 9 | Price bands | Dynamic % band around mark price | Fat finger protection. Exact % TBD per instrument. |
| 10 | Stop trigger sources | Mark price (default), last traded, index price | User-selectable. Adopted from Backpack. Mark \= safest default, last \= most reactive, index \= most manipulation-resistant. |
| 11 | API authentication | ED25519 asymmetric signatures | Stronger than HMAC-SHA256 (Binance/Bybit). Secret key never sent to server. Natural for Solana ecosystem. Adopted from Backpack. |

### Open Questions — Flow 2

- [ ] Price band percentage per instrument — TBD (likely 5% for BTC, tighter for XAU)  
- [ ] Rate limits — exact numbers per user tier TBD  
- [ ] Position limits — per-user and market-wide limits TBD  
- [ ] WAL storage — append to local disk, replicate to standby, or use a distributed log (Kafka)?  
- [ ] Quantity-decrease modification preserving priority — worth adding for MVP?  
- [ ] Maximum open orders per user — cap to prevent book spam?

---

## Architecture Decisions: Adopted from Industry Research

After researching TradFi exchanges (CME, NSE, Eurex, ICE), crypto CEXes (Deribit, Binance, Backpack, Bybit, OKX), and crypto DEXes (Hyperliquid, dYdX, Drift, Vertex), these are the key design decisions adopted for Asgard FnO, with full reasoning.

### Sqrt-Based Margin Scaling

**Adopted from:** Backpack Exchange

**What it is:** Instead of a flat margin rate (e.g., 10% for everyone), margin requirements scale with position size using a square root function. Small traders get full leverage. Large positions auto-require more margin because liquidating them has more market impact.

**The formula:**

IMF (Initial Margin Fraction) \= max(base\_IMF, imf\_factor × √notional)

IM  \= IMF × notional

MMF (Maintenance Margin Fraction) \= max(base\_MMF, mmf\_factor × √notional)

MM  \= MMF × notional

**How it works in practice:**

Parameters (example for BTC-USDC):

  base\_IMF \= 0.01 (1%)       base\_MMF \= 0.005 (0.5%)

  imf\_factor \= 0.00003        mmf\_factor \= 0.000015 (half of imf\_factor — MM is always \~half of IM)

Small trader: 1 BTC ($50,000 notional)

  IMF \= max(0.01, 0.00003 × √50000) \= max(0.01, 0.0067) \= 0.01 (1%)

  IM  \= $500

  → 100x leverage available (at base parameters — actual max leverage TBD)

  ⚠ CALIBRATION NOTE: At 1% IM / 0.5% MM, a 1% adverse move wipes the

  entire margin and the IM-to-MM buffer is only 0.5%. Combined with a

  1% liquidation fee, positions at max leverage are guaranteed to go

  bankrupt on liquidation (fee alone exceeds the MM buffer). The base\_IMF,

  base\_MMF, and liquidation\_fee parameters MUST be calibrated together.

  Options: raise base\_IMF (e.g., 2-5%), raise base\_MMF, or lower the

  liquidation fee for small positions. Final values require backtesting

  against historical BTC volatility.

Medium trader: 50 BTC ($2,500,000 notional)

  IMF \= max(0.01, 0.00003 × √2500000) \= max(0.01, 0.047) \= 0.047 (4.7%)

  IM  \= $117,500

  → \~21x effective leverage

Whale: 500 BTC ($25,000,000 notional)

  IMF \= max(0.01, 0.00003 × √25000000) \= max(0.01, 0.15) \= 0.15 (15%)

  IM  \= $3,750,000

  → \~6.7x effective leverage

**Why sqrt and not linear?** Linear scaling (margin \= constant × notional) doesn't account for the non-linear relationship between position size and liquidation impact. A 500 BTC liquidation doesn't have 500x the market impact of 1 BTC — it has much more because of order book depth constraints. Sqrt captures this sublinear-but-increasing relationship.

**Implementation is one function per direction:**

fn calculate\_margin\_fraction(notional: f64, base: f64, factor: f64) \-\> f64 {

    f64::max(base, factor \* notional.sqrt())

}

Parameters (`base_IMF`, `imf_factor`, `base_MMF`, `mmf_factor`) are per-instrument config values. The engine logic doesn't change — it just calls this function instead of using a flat percentage.

**How TradFi compares:** CME's SPAN methodology achieves a similar effect through scenario-based analysis — larger positions naturally produce larger worst-case losses in the 16 scenarios. Sqrt scaling is a simpler approximation of the same principle.

---

### Mark Price Formula

**Adopted from:** Backpack Exchange

**What it is:** The mark price is the "fair" price used for all margin calculations, liquidation triggers, and unrealized PnL. It must be manipulation-resistant (can't be moved by a single large trade) but still responsive to real market conditions.

**The formula:**

Mark Price \= Index Price \+ 1-minute EWMA of (mid\_price \- index\_price)

Bounded: Mark Price ∈ \[Index × 0.95, Index × 1.05\]

Where:

- **Index Price** \= oracle price (Pyth/Switchboard), aggregated from multiple spot exchanges  
- **mid\_price** \= (best\_bid \+ best\_ask) / 2 on our orderbook  
- **EWMA** \= Exponential Weighted Moving Average with a 1-minute halflife (α ≈ 0.0116 per second, meaning \~50% of the weight is on the last 60 seconds of data). Smooths out noise while remaining responsive.  
- **±5% bound** \= mark price can never deviate more than 5% from the oracle, no matter what happens on our book

**Why this design:**

| Component | What it does | Why |
| :---- | :---- | :---- |
| Index price (oracle) | Anchor. Resistant to manipulation on any single venue. | Prevents someone from moving our mark price by trading on our book alone. |
| \+ EWMA of (mid \- index) | Adjusts for our market's actual supply/demand. If our book consistently trades at a premium/discount to index, mark price reflects that. | Makes margin calculations more accurate for positions on our exchange specifically. |
| ±5% bound | Hard cap on deviation from oracle. | If our book is thin and someone spikes the mid-price, the mark price won't follow beyond 5%. Prevents liquidation cascades from manipulation. |

**Fallback hierarchy** (if data is unavailable, adopted from Backpack):

1\. Index \+ EWMA of (mid \- index)        ← normal operation

2\. Index price alone                     ← if our orderbook has no quotes

3\. Median of {best bid, best ask, last}  ← if oracle is stale (SECONDS, not minutes)

4\. Mid price (best bid \+ ask / 2\)        ← degraded

5\. Last traded price                     ← emergency only

**Important caveat:** Levels 3–5 use our own orderbook data as the price source. This removes the oracle's manipulation protection — someone could place orders on our thin book to move the mark price and trigger liquidations. These fallbacks are acceptable for **momentary** degradation (a few seconds of oracle staleness). For sustained oracle outage (\>30 seconds), the system enters ORACLE\_HALT and freezes mark price instead of falling through to these levels. See Flow 9 for the full oracle failure procedure.

---

### Insurance Vault Design

**Adopted from:** Drift Protocol (open vault model) \+ CME/Eurex (SITG principle) \+ Backpack (liquidation waterfall)

#### How TradFi handles this

Every TradFi CCP maintains a guarantee fund. Key facts:

- **CME**: \~$9.4B fund \+ $250M of CME's own capital (SITG) at risk  
- **NSE Clearing**: ~~₹11,400 Cr (~~$1.35B). SEBI mandates clearing corp funds 50%, exchange 25%, members 25%.  
- **Eurex**: \~EUR 8.7B \+ EUR 143M SITG  
- **No external deposits for yield.** Fund participation is mandatory for clearing members, not an investment product.  
- **Default waterfall**: defaulter's margin → defaulter's fund contribution → CCP's SITG → non-defaulting members' fund → assessments → CCP equity

The critical principle: **the CCP puts its own capital on the line (SITG) before touching anyone else's money.**

#### How crypto handles this

Three models exist:

**Model A — Exchange-funded only (Deribit, Binance):** Exchange accumulates a fund from fees. No external deposits. Binance SAFU: \~$1.3B. Simple, but capital sits idle.

**Model B — Open vault (Hyperliquid HLP, Drift, GMX):** Anyone deposits, earns yield, takes risk. Hyperliquid HLP: \~$230M, nearly drained in JELLY incident (March 2025). Drift: separate per-market vaults, 13-day cooldown, 80% utilization gate.

**Model C — Institutional BLP (Backpack):** Select institutional MMs only. Absorb distressed positions at favorable prices. Not a yield product — profit from flipping flow.

#### Our design: Hybrid of the best elements

Insurance Vault (USDC pool)

│

├── Depositors:

│   ├── Exchange (SITG — first deposit, permanently locked, cannot withdraw)

│   ├── External depositor A

│   ├── External depositor B

│   └── ...anyone can deposit

│

├── Revenue IN:

│   └── Liquidation fees (1% of every liquidation fill)

│       → distributed proportionally to depositors as APY

│

├── Losses OUT:

│   └── Bankrupt liquidation deficits ONLY

│       (gap between liquidation price and bankruptcy price)

│       NOT counterparty to trades (unlike HLP — bounded, predictable risk)

│

├── Withdrawal rules (utilization gate, adopted from Drift):

│   ├── Normal (utilization \< 50%):  7-day cooldown, then withdraw freely

│   ├── Elevated (50-80%):           14-day cooldown, partial only (max 25% per withdrawal)

│   ├── Stressed (\> 80%):            Withdrawals BLOCKED entirely

│   └── No rewards during cooldown period

│

└── Exchange's SITG:

    └── Permanently locked. Cannot withdraw. Ever.

        Signals maximum commitment to users and depositors.

**Why the utilization gate (from Drift) and not a fixed cooldown:**

| Mechanism | What happens during a crash | Verdict |
| :---- | :---- | :---- |
| Fixed cooldown (e.g., 7 days) | People start cooldown and wait. $293M fled HLP over 2 weeks despite 4-day lockup. | Doesn't prevent bank run, just spreads it. |
| Utilization gate (Drift) | Withdrawals blocked when fund is \>80% utilized. Capital stays when it's needed most. Unblocks when stress passes. | Actually works. Only mechanism that held during stress. |
| Long lockup (e.g., 90 days, Maple) | LPs trapped during the exact event they're insuring against. Sherlock lost $4M. | Backfired. Maple scrapped it in V2. |

**Utilization calculation:**

utilization \= total\_losses\_absorbed / (total\_vault\_balance \+ total\_losses\_absorbed)

Note: denominator is the ORIGINAL balance (before losses), not the current balance.

This prevents the ratio from spiking disproportionately as the vault shrinks.

Worked example:

  Vault starts at $200K (exchange $100K SITG \+ $100K from depositors)

  Bankrupt liquidation: $50K deficit absorbed by vault.

  current\_balance \= $150K

  total\_losses\_absorbed \= $50K

  original\_balance \= $150K \+ $50K \= $200K

  utilization \= $50K / $200K \= 25% → NORMAL (7-day cooldown for withdrawals)

  Another $80K deficit:

  current\_balance \= $70K

  total\_losses\_absorbed \= $130K

  original\_balance \= $70K \+ $130K \= $200K

  utilization \= $130K / $200K \= 65% → ELEVATED (14-day cooldown, partial only)

  Another $50K deficit:

  current\_balance \= $20K

  total\_losses\_absorbed \= $180K

  utilization \= $180K / $200K \= 90% → STRESSED \+ EMERGENCY trigger

fn can\_withdraw(vault: \&InsuranceVault, depositor: \&Depositor) \-\> bool {

    let original\_balance \= vault.current\_balance \+ vault.total\_losses\_absorbed;

    let utilization \= vault.total\_losses\_absorbed / original\_balance;

    if utilization \> 0.80 {

        return false;  // blocked — vault is under stress

    }

    let cooldown\_days \= if utilization \> 0.50 { 14 } else { 7 };

    let cooldown\_passed \= depositor.cooldown\_start \+ cooldown\_days \<= now();

    if utilization \> 0.50 {

        // elevated — partial only, max 25% of deposit per withdrawal

        cooldown\_passed && withdrawal\_amount \<= depositor.balance \* 0.25

    } else {

        // normal — full withdrawal after cooldown

        cooldown\_passed

    }

}

**Per-instrument vaults (V1.1, adopted from Drift):**

For MVP with only BTC-USDC and XAU-USDC, a single vault is fine. When we add more instruments, split into per-instrument (or per-category) vaults:

BTC-USDC Insurance Vault  ← BTC liquidation losses only

XAU-USDC Insurance Vault  ← XAU liquidation losses only

This prevents contagion — one volatile market blowing up doesn't drain another market's backstop. Drift does this and it's one of their best design choices.

**Regulatory note:** An open vault where anyone deposits USDC and earns yield from liquidation fees may be classified as a securities/investment product in some jurisdictions (Howey test in the US, MiCA in the EU). The yield-bearing nature makes it different from a simple insurance fund. Legal review is required before launch to determine: (a) whether the vault requires separate licensing, (b) whether depositors need to be accredited/qualified, (c) geographic restrictions. This is a business decision that affects the vault's design (open vs restricted participation).

---

### Liquidation Model

**Adopted from:** Backpack (gradual/partial liquidation \+ waterfall) \+ Drift (vault model)

#### Gradual/Partial Liquidation

Instead of liquidating an entire position at once, we liquidate incrementally — only the minimum portion needed to restore healthy margin.

**How Backpack does it:**

- When MMR hits 100%, cancel all user's open orders first (frees margin)  
- Then liquidate \~10% of position per iteration  
- 1-second ticks between iterations  
- After each iteration, re-check: is margin restored? If yes, stop liquidating.  
- Price bands prevent execution at extreme prices

**Why this is better than full liquidation:**

Full liquidation:

  User has 10 BTC position, margin dips 2% below MM.

  Engine liquidates ALL 10 BTC.

  10 BTC market sell hammers the orderbook.

  Price drops further. Cascading liquidations.

  User loses entire position unnecessarily.

Gradual liquidation:

  User has 10 BTC position, margin dips 2% below MM.

  Engine liquidates 1 BTC (10%).

  Re-check: margin restored? Maybe yes — user keeps 9 BTC.

  If not, liquidate another 1 BTC. Repeat.

  Minimal market impact. User keeps as much as possible.

**Our implementation:**

Liquidation loop:

  1\. Cancel all user's open orders (frees open\_order\_margin)

  2\. Re-check equity vs MM. If restored → STOP, no liquidation needed.

  3\. Calculate liquidation chunk: \~10% of position (configurable per instrument)

  4\. Place reduce-only IOC order for the chunk on the orderbook

  5\. Wait for fill (1-second tick)

  6\. Re-check equity vs MM. If restored → STOP.

  7\. If not restored → repeat from step 3

  8\. If orderbook can't absorb → insurance vault covers the deficit

  9\. If insurance vault depleted → ADL

#### Liquidation Waterfall

Layer 1: ORDERBOOK

  Liquidation order placed as reduce-only IOC on the public book.

  Handles \~99%+ of liquidations in normal conditions.

  │

  │ if orderbook can't absorb (price impact too extreme,

  │ or position goes bankrupt before fully closed)

  ▼

Layer 2: INSURANCE VAULT

  Vault absorbs the deficit when a position is closed at a price worse

  than the bankruptcy price (the price where equity \= 0).

  Bankruptcy price (for reference):

    Long:  bankruptcy\_price \= entry\_price \- (margin / size)

    Short: bankruptcy\_price \= entry\_price \+ (margin / size)

  If the liquidation execution price is worse than the bankruptcy price,

  the gap (execution\_price vs bankruptcy\_price × size) is the deficit.

  The vault covers this deficit.

  Funded by liquidation fees (1%) \+ external depositors earning APY.

  │

  │ if vault is depleted

  ▼

Layer 3: ADL (Auto-Deleveraging) — LAST RESORT

  Force-close profitable traders on the opposite side.

  Ranked by: PnL% × effective leverage (highest priority first).

  Should never trigger on a well-run exchange.

  Deribit has not triggered socialized loss in recent years (post-2020).

V1.1 addition:

  Layer 1.5: BLP (Backstop Liquidity Providers)

  Institutional MMs absorb distressed positions at favorable pricing.

  Inserted between orderbook and insurance vault.

#### Liquidation Fee

**1% of liquidation fill notional** (adopted from Backpack).

Liquidation fee distribution:

  → 100% flows into the insurance vault

  → Becomes APY for vault depositors

  → Creates a self-reinforcing loop: more liquidations \= more fees

    \= higher APY \= more depositors \= bigger safety net

---

### Fee Structure (Reference Benchmark)

Based on Backpack's fee schedule (our closest comparable):

**Futures trading fees:**

| Tier | 30-day Volume | Maker | Taker |
| :---- | :---- | :---- | :---- |
| Base | \< $1M | 0.020% | 0.050% |
| Tier 2 | ≥ $1M | 0.015% | 0.040% |
| Tier 3 | ≥ $5M | 0.010% | 0.030% |
| VIP | ≥ $25M | 0.000% | 0.018% |

**MM program (Backpack reference):**

- $300K/month rewards ($200K futures \+ $100K spot)  
- 0% maker fee in first month  
- Futures rebates credited instantly after every fill  
- Scoring: 80% volume \+ 20% liquidity  
- Liquidity KPIs: $50K one-sided depth at ≤2.5bps spread for major pairs

**Our fee structure:** TBD — will use these as reference but adjust based on our market positioning. Key principles:

- Maker-taker model (incentivize liquidity provision)  
- Competitive with Backpack/Deribit  
- MM program with obligations (depth, spread) and rewards (rebates, fee reduction)

---

### Decisions Summary — Industry Research

| \# | Decision | Choice | Source | Rationale |
| :---- | :---- | :---- | :---- | :---- |
| 1 | Margin scaling | Sqrt-based: IMF \= max(base, factor × √notional) | Backpack | Prevents under-margining large positions. One function, parametric per instrument. |
| 2 | Mark price | Index \+ 1-min EWMA of (mid \- index), bounded ±5% | Backpack | Oracle-anchored, market-aware, manipulation-resistant. Fallback hierarchy for degraded data. |
| 3 | Insurance vault | Open vault, anyone deposits, earns APY from liquidation fees | Drift | Better than exchange-only (more capital) or HLP (too much risk). Exchange SITG permanently locked. |
| 4 | Vault withdrawals | Utilization gate: \<50% \= 7d, 50-80% \= 14d partial, \>80% \= blocked | Drift | Only mechanism that actually prevents capital flight during stress (proven). |
| 5 | Liquidation style | Gradual/partial, \~10% per iteration, minimum needed | Backpack | Less market impact, better UX, reduces insurance fund drain. |
| 6 | Liquidation waterfall | Orderbook → insurance vault → ADL | Backpack \+ Drift | Three layers. ADL is absolute last resort. BLP added at V1.1. |
| 7 | Liquidation fee | 1% of fill notional → insurance vault | Backpack | Funds the vault. Creates APY for depositors. Self-reinforcing safety net. |
| 8 | API auth | ED25519 asymmetric signatures | Backpack | Stronger than HMAC. Natural for Solana ecosystem. |
| 9 | Reduce-Only orders | Flag on any order type | Backpack | Prevents accidental position opens. Simple, essential. |
| 10 | Stop trigger sources | Mark (default), last traded, index — user selects | Backpack | Flexibility for different risk management strategies. |
| 11 | Exchange SITG | Permanently locked in vault. Cannot withdraw. | CME, Eurex | Non-negotiable trust signal. Exchange capital at risk first. |
| 12 | Per-instrument vaults | V1.1 — separate insurance vaults per market | Drift | Prevents contagion. One market blowing up doesn't drain another's backstop. |

### Open Questions — Industry Research

- [ ] Per-instrument vaults vs single vault for MVP — single vault likely fine with only BTC \+ XAU?  
- [ ] Exact sqrt parameters (base\_IMF, imf\_factor) per instrument — TBD, calibrate against CME SPAN margins  
- [ ] Insurance vault APY display — show real-time APY to attract depositors?  
- [ ] Liquidation iteration percentage — 10% default, should it be configurable per instrument?  
- [ ] Multi-layer price bands (Backpack has 4 layers) — adopt for V1.1?  
- [ ] Fee structure — exact numbers, tier thresholds  
- [ ] MM program — exact KPI requirements, reward structure

---

## Flow 3: Order Matching → Trade

### Overview

Two orders cross in the matching engine. A trade happens. This flow covers everything from the moment of the match to when both parties see their updated positions, PnL, and margin.

This is where the exchange's core accounting lives — the zero-sum property of futures. Every dollar one side gains, the other side loses. Getting this wrong means money appears or disappears from the system.

### What the matching engine produces

When a buy and sell order cross, the matching engine emits a trade event:

TradeEvent {

    trade\_id:       "T-00001"

    instrument:     "BTC-USDC-20260327"

    price:          50100

    quantity:        1

    buyer:          alice

    buyer\_order\_id: "O-abc"

    buyer\_role:     TAKER       ← alice's order crossed the book

    seller:         carol

    seller\_order\_id: "O-xyz"

    seller\_role:    MAKER       ← carol's order was resting

    timestamp:      1711036800000

    sequence:       1275

}

**Maker vs Taker:**

- **Maker** \= the resting order that was already in the book. Provides liquidity. Pays lower fees.  
- **Taker** \= the incoming order that crossed the spread. Removes liquidity. Pays higher fees.

The maker had capital committed in the book providing liquidity. The taker consumed that liquidity. This distinction drives fee incentives.

### What happens after the match

Everything below happens atomically — all steps complete or none do. No partial state updates.

Trade Event

    │

    ├──► 1\. Fee Calculation

    │

    ├──► 2\. Position Update (buyer)

    │       ├── New position? → Create

    │       ├── Increasing? → Update avg entry, add size

    │       ├── Reducing? → Realize PnL, reduce size

    │       └── Closing/Flipping? → Realize PnL, close, maybe open reverse

    │

    ├──► 3\. Position Update (seller)

    │       └── (same logic as buyer)

    │

    ├──► 4\. Margin Recalculation (both parties)

    │       ├── Release open\_order\_margin for filled orders

    │       ├── Calculate new position\_margin (sqrt-based)

    │       └── Update equity, available\_balance

    │

    ├──► 5\. Broadcast

    │       ├── Execution reports → buyer, seller (WebSocket)

    │       ├── Trade data → all market data subscribers

    │       ├── Book update → all subscribers (depth changed)

    │       └── Audit trail → event log

    │

    └──► 6\. Risk Engine Check

            └── Are any positions now undercollateralized?

                (margin requirements changed because position sizes changed)

### Step 1: Fee Calculation

Maker fee \= quantity × price × maker\_fee\_rate

Taker fee \= quantity × price × taker\_fee\_rate

Example: trade at 50100, qty 1

  Carol (maker): 1 × 50100 × 0.00020 \= 10.02 USDC

  Alice (taker): 1 × 50100 × 0.00050 \= 25.05 USDC

Fees deducted from wallet\_balance immediately at fill time.

Fee revenue credited to exchange fee\_revenue account.

Fees are charged separately — they do NOT affect the fill price.

Both maker and taker receive the fill at the trade price;

fees are deducted as a separate wallet\_balance adjustment.

### Step 2 & 3: Position Updates

Four cases for each side of the trade:

**Case A — Opening a new position (no existing position)**

Before: alice has no BTC-USDC-20260327 position

Trade:  alice buys 1 lot at 50100

After:

  alice.position \= {

    instrument:   "BTC-USDC-20260327"

    side:         LONG

    size:         1

    entry\_price:  50100

    margin\_mode:  cross (or isolated)

  }

**Case B — Increasing an existing position (same direction)**

Before: alice is long 2 lots, entry\_price \= 50000

Trade:  alice buys 1 more lot at 50100

After:

  new\_entry \= (50000 × 2 \+ 50100 × 1\) / (2 \+ 1\) \= 50033.33

  alice.position \= {

    side:         LONG

    size:         3

    entry\_price:  50033.33     ← weighted average

  }

**Case C — Reducing an existing position (opposite direction, partial)**

Before: alice is long 3 lots, entry\_price \= 50000

Trade:  alice sells 1 lot at 50200

After:

  Realized PnL \= (50200 \- 50000\) × 1 \= \+200 USDC

  alice.wallet\_balance \+= 200

  alice.position \= {

    side:         LONG

    size:         2              ← reduced

    entry\_price:  50000          ← unchanged (average entry stays)

  }

**Case D — Closing and flipping (opposite direction, exceeds position)**

Before: alice is short 2 lots, entry\_price \= 50500

Trade:  alice buys 5 lots at 50200

After:

  Step 1: Close the short (2 lots)

    Realized PnL \= (50500 \- 50200\) × 2 \= \+600 USDC

    alice.wallet\_balance \+= 600

  Step 2: Open new long (5 \- 2 \= 3 lots)

    alice.position \= {

      side:         LONG

      size:         3

      entry\_price:  50200

    }

### The zero-sum invariant

Futures are zero-sum. The engine must maintain this at all times:

Σ trading\_pnl (all users) \+ Σ unrealized\_pnl (all open positions) \= 0

Where trading\_pnl \= realized PnL from position closes ONLY (excluding fees).

Fees are a separate extraction: they flow from user wallet\_balance to fee\_revenue.

Liquidation fees flow from user wallet\_balance to insurance vault.

The full accounting identity:

  Σ wallet\_balance (all users)

\+ Σ unrealized\_pnl (all open positions)

\+ fee\_revenue\_balance

\+ insurance\_vault\_balance

\= Σ total\_deposited (all users, all time) \- Σ total\_withdrawn (all users, all time)

If this ever diverges, there's an accounting bug. The reconciliation engine checks this invariant continuously.

Unrealized PnL for any open position:

Long:  unrealized\_pnl \= (mark\_price \- entry\_price) × size

Short: unrealized\_pnl \= (entry\_price \- mark\_price) × size

For every long there's a short. When mark price moves, longs gain exactly what shorts lose (and vice versa). The system net is always zero.

### Step 4: Margin Recalculation

After every trade, both parties' margin state changes.

**For the taker (alice, bought 1 lot — new position):**

Before trade:

  open\_order\_margin: 100 USDC (reserved for the buy order)

  position\_margin:   0 (no existing position)

After trade:

  open\_order\_margin: 0 (order filled, reservation released)

  New position notional: 1 × 50100 \= 50100 USDC

  IMF \= max(base\_IMF, imf\_factor × √50100)

      \= max(0.01, 0.00003 × 223.8)

      \= max(0.01, 0.0067) \= 0.01

  position\_margin \= 0.01 × 50100 \= 501 USDC

  Net margin change: \-100 (released) \+ 501 (new position) \= \+401 USDC locked

  available\_balance \-= 401

**For a reducing trade (alice closing part of a position):**

Before trade:

  position: long 3 lots, entry 50000, notional 150000

  Old IMF \= max(0.01, 0.00003 × √150000) \= 0.0116

  Old position\_margin \= 0.0116 × 150000 \= 1740

After selling 1 lot at 50200:

  Realized PnL: \+200 → wallet\_balance \+= 200

  position: long 2 lots, entry 50000, notional 100000

  New IMF \= max(0.01, 0.00003 × √100000) \= 0.01

  New position\_margin \= 0.01 × 100000 \= 1000

  Margin freed: 1740 \- 1000 \= 740 USDC

  available\_balance \+= 740 \+ 200 (realized PnL) \= \+940

Note the sqrt effect: reducing from 3 lots to 2 lots freed 740 USDC of margin, not just 1/3 of the original margin (580). The sqrt curve means smaller positions are proportionally cheaper to margin — the reduction benefit is nonlinear.

### Step 5: Broadcast

All happen in parallel:

**a) Execution reports to both parties (private, WebSocket):**

To alice (buyer/taker):

{

  type: "fill",

  trade\_id: "T-00001",

  order\_id: "O-abc",

  side: "buy",

  price: 50100,

  quantity: 1,

  fee: 25.05,

  fee\_rate: 0.00050,

  role: "taker",

  realized\_pnl: 0,

  position: {

    side: "long",

    size: 1,

    entry\_price: 50100,

    unrealized\_pnl: 0,

    liquidation\_price: 40651,       // see note below

    margin: 501

  },

  balance: {

    wallet\_balance: 9974.95,        // 10000 \- 25.05 fee

    equity: 9974.95,                // wallet\_balance \+ unrealized\_pnl (0)

    available\_balance: 9473.95      // equity \- position\_margin (501)

  },

  timestamp: 1711036800000

}

// Liquidation price derivation (cross-margin, single position, no other positions):

// liq\_price ≈ entry \- (equity \- MM) / size

// MM \= MMF × notional ≈ 0.005 × 50100 \= 250.50

// liq\_price ≈ 50100 \- (9974.95 \- 250.50) / 1 \= 50100 \- 9724.45 \= 40375.55

// With the full equity as cushion, liquidation is very far away.

// Note: this is approximate — the exact calculation requires numerical

// solving because MM itself changes as price moves toward liquidation

// (notional changes → MMF changes via sqrt scaling).

**b) Trade data to all subscribers (public, WebSocket):**

{

  type: "trade",

  instrument: "BTC-USDC-20260327",

  price: 50100,

  quantity: 1,

  side: "buy",

  timestamp: 1711036800000

}

No user identities in public data. Only price, quantity, taker side, timestamp.

**c) Orderbook update (public, WebSocket):**

The filled maker order is removed from the book. Depth at that price level changes.

**d) Ticker update (public, WebSocket):**

Last price, 24h volume, high/low, open interest — all updated.

**e) Audit trail:**

Full trade details with both user IDs, fees, sequence number. Immutable log.

### Step 6: Post-Trade Risk Check

After every trade, the risk engine scans both parties:

For each party:

  equity \= wallet\_balance \+ unrealized\_pnl

  total\_MM \= Σ MMF(notional) × notional  (across all positions, sqrt-based)

  if equity \<= total\_MM:

    → trigger liquidation (Flow 5\)

  if equity \<= total\_IM × 1.1:

    → send margin warning to user via WebSocket

This catches edge cases:

- A fill at a worse price than expected (market order sweeping multiple levels)  
- A trade that flips a position (new position might need more margin than the old)  
- Fee deduction pushing equity below MM  
- A partial fill that changes the margin curve (sqrt nonlinearity)

### How TradFi compares

|  | TradFi (CME/NSE) | Asgard |
| :---- | :---- | :---- |
| Trade registration | CCP novates: Buyer↔CCP, Seller↔CCP | No novation — internal ledger, direct user accounts |
| Position tracking | Clearing member → client account hierarchy | Flat: user → position. No member layer. |
| Margin recalculation | SPAN runs end-of-day (with intraday calls) | Real-time after every trade (continuous) |
| Fee model | Exchange fee \+ clearing fee \+ NFA fee \+ ... | Single maker/taker fee |
| PnL settlement | Daily variation margin cash transfers | Continuous — realized PnL credits instantly |
| Trade publication | MDP 3.0 (UDP multicast, SBE encoding) | WebSocket JSON |

The biggest difference: TradFi settles PnL once per day (variation margin). We settle continuously — unrealized PnL updates every tick, realized PnL credits instantly on trade. Standard in crypto, better UX.

### Complete flow diagram

┌────────────────────────────────────────────────────────────────────┐

│                        MATCHING ENGINE                              │

│                                                                     │

│  Buy order (alice)          Sell order (carol)                      │

│  limit 50100, qty 2         limit 50100, qty 1 (resting)           │

│       │                           │                                 │

│       └──────────┬────────────────┘                                │

│                  │                                                  │

│            MATCH: 1 lot @ 50100                                    │

│            alice \= TAKER, carol \= MAKER                            │

│            remaining: alice 1 lot rests in book                    │

│                  │                                                  │

└──────────────────┼─────────────────────────────────────────────────┘

                   │

                   ▼

┌──────────────────────────────────────────────────────────────────┐

│                    POST-TRADE PROCESSING                          │

│                    (atomic — all or nothing)                       │

│                                                                   │

│  ┌─────────────────────┐                                         │

│  │  1\. Fee Calculation  │                                        │

│  │  alice: 25.05 USDC  │                                        │

│  │  carol: 10.02 USDC  │                                        │

│  └──────────┬──────────┘                                         │

│             ▼                                                     │

│  ┌──────────────────────────────────────────┐                    │

│  │  2\. Position Updates                      │                   │

│  │                                           │                   │

│  │  alice: new long 1 @ 50100               │                   │

│  │    wallet\_balance \-= 25.05 (fee)          │                   │

│  │    open\_order\_margin released             │                   │

│  │    position\_margin \= 501 USDC             │                   │

│  │                                           │                   │

│  │  carol: new short 1 @ 50100              │                   │

│  │    wallet\_balance \-= 10.02 (fee)          │                   │

│  │    open\_order\_margin released             │                   │

│  │    position\_margin \= 501 USDC             │                   │

│  └──────────┬───────────────────────────────┘                    │

│             ▼                                                     │

│  ┌─────────────────────────────┐                                 │

│  │  3\. Margin Recalculation    │                                 │

│  │  Both: equity,              │                                 │

│  │  available\_balance updated  │                                 │

│  └──────────┬──────────────────┘                                 │

│             ▼                                                     │

│  ┌─────────────────────────────┐                                 │

│  │  4\. Risk Check              │                                 │

│  │  equity vs MM for both      │                                 │

│  │  → liquidation if breached  │                                 │

│  │  → warning if approaching   │                                 │

│  └──────────┬──────────────────┘                                 │

│             ▼                                                     │

│  ┌─────────────────────────────────────────────────────┐         │

│  │  5\. Broadcast (parallel)                             │        │

│  │  • Execution reports → alice, carol (private WS)     │        │

│  │  • Trade → all subscribers (public WS)               │        │

│  │  • Book update → all subscribers (public WS)         │        │

│  │  • Ticker → all subscribers (public WS)              │        │

│  │  • Audit trail → event log                           │        │

│  └─────────────────────────────────────────────────────┘         │

└──────────────────────────────────────────────────────────────────┘

---

### Decisions Made — Flow 3

| \# | Decision | Choice | Rationale |
| :---- | :---- | :---- | :---- |
| 1 | Post-trade processing | Atomic — all steps succeed or none | No partial state. System inconsistency if any step fails mid-way. |
| 2 | Entry price tracking | Weighted average | new\_entry \= (old × old\_qty \+ fill × fill\_qty) / total\_qty. Simpler than FIFO. Used by Binance, Bybit, Backpack. |
| 3 | PnL realization | Immediate on reduce/close | Realized PnL credited to wallet\_balance instantly. No daily settlement cycle. |
| 4 | Fee deduction | From wallet\_balance at trade time | Immediate. Maker pays less than taker. Flows to exchange fee\_revenue. |
| 5 | Margin recalculation | After every trade, sqrt-based | Position margin recalculated with new size. Open order margin released for filled quantity. |
| 6 | Post-trade risk check | Immediate after every trade | Catches bad fills, position flips, fee-driven margin breach. |
| 7 | Trade publication | Anonymous — no user IDs in public data | Standard practice. Price, quantity, taker side, timestamp only. |
| 8 | Zero-sum invariant | Σ realized \+ Σ unrealized \= 0, checked continuously | Core accounting invariant. Divergence \= bug. |

### Open Questions — Flow 3

- [ ] Open interest tracking — increment on new position open, decrement on close. Publish via market data and ticker.  
- [ ] Trade ID format — sequential integer? UUID? Needs to be unique and sortable.  
- [ ] Margin warning threshold — at what equity/IM ratio do we warn? 1.1x IM? 1.2x?  
- [ ] Liquidation price estimation — show estimated liq price in execution report? Recalculates on every trade and mark price change.  
- [ ] Partial fill handling — large order filling across multiple resting orders: each fill is a separate TradeEvent, processed sequentially within the same atomic batch.

---

## Flow 4: Position Lifecycle & Margin

### Overview

Flows 2 and 3 covered how positions are created (order → match → trade). Flow 5 covers how they die violently (liquidation). This flow covers everything in between — the continuous life of a position as the market moves.

### What the Risk Engine does every tick

The risk engine is a continuous loop. Every time the mark price updates (sub-second), it recalculates every user's risk state:

On every mark price update:

  For each user with open positions:

    1\. Recalculate unrealized\_pnl per position

    2\. Recalculate equity

    3\. Compare equity vs maintenance margin

    4\. Decide: safe / warning / liquidation

### Position state model

                    ┌──────────┐

                    │  (none)  │  user has no position in this instrument

                    └────┬─────┘

                         │ trade fills (Flow 3\)

                         ▼

                    ┌──────────┐

              ┌────►│   OPEN   │◄────────────────────────────┐

              │     └────┬─────┘                              │

              │          │                                     │

              │          ├── mark price moves                  │

              │          │   → unrealized\_pnl changes          │

              │          │   → equity changes                  │

              │          │   → risk state recalculated         │

              │          │                                     │

              │          ├── user adds to position (trade)     │

              │          │   → size increases, entry repriced ─┘

              │          │

              │          ├── user partially closes (trade)

              │          │   → size decreases, PnL realized

              │          │   → still OPEN (smaller) ───────────┘

              │          │

              │          ├── equity \< MM → LIQUIDATING (Flow 5\)

              │          │

              │          ├── user fully closes → CLOSED

              │          │

              │          └── contract expires → SETTLED (Flow 6\)

              │

              │     ┌──────────────┐

              │     │ LIQUIDATING  │  gradual liquidation in progress

              │     └──────┬───────┘

              │            ├── margin restored → back to OPEN (reduced)

              │            └── fully liquidated → CLOSED

              │

              │     ┌──────────┐

              └─────│  CLOSED  │  position gone, all margin freed

                    └──────────┘

                    ┌──────────┐

                    │ SETTLED  │  expired contract, final PnL realized

                    └──────────┘

### The continuous margin check

Every mark price update triggers this for every user:

mark\_price updates to 50300

For user alice (long 2 BTC-USDC @ 50000, cross-margin):

  Step 1: Recalculate unrealized PnL

    unrealized\_pnl \= (50300 \- 50000\) × 2 \= \+600 USDC

  Step 2: Recalculate equity

    equity \= wallet\_balance \+ unrealized\_pnl \= 5000 \+ 600 \= 5600

  Step 3: Calculate maintenance margin (sqrt-based)

    notional \= 2 × 50300 \= 100600

    MMF \= max(base\_MMF, mmf\_factor × √100600) \= max(0.005, 0.00476) \= 0.005

    total\_MM \= 0.005 × 100600 \= 503 USDC

  Step 4: Calculate initial margin

    IMF \= max(0.01, 0.00003 × √100600) \= max(0.01, 0.00951) \= 0.01

    total\_IM \= 0.01 × 100600 \= 1006 USDC

  Step 5: Determine risk state

    if equity \<= total\_MM:           → LIQUIDATE

    elif equity \<= total\_IM × 1.1:   → WARNING

    else:                            → SAFE

    Alice: 5600 \>\> 503 → SAFE

### Cross-margin vs isolated: how they differ

**Cross-margin — entire account is one pool:**

Alice: wallet\_balance 10000, long 1 BTC @ 50000, short 5 XAU @ 2000

BTC mark \= 49000, XAU mark \= 2050

  unrealized\_BTC \= (49000 \- 50000\) × 1 \= \-1000

  unrealized\_XAU \= (2000 \- 2050\) × 5 \= \-250

  equity \= 10000 \+ (-1250) \= 8750

  total\_MM ≈ 296

  8750 \>\> 296 → SAFE (large balance absorbs losses, positions survive)

**Isolated-margin — each position ring-fenced:**

Same positions but isolated: BTC allocated 600, XAU allocated 200

Free balance: 10000 \- 600 \- 200 \= 9200

  BTC position: effective\_equity \= 600 \+ (-1000) \= \-400 \< MM → LIQUIDATE

  XAU position: effective\_equity \= 200 \+ (-250) \= \-50 \< MM → LIQUIDATE

  Free balance: 9200 → UNTOUCHED

Both positions liquidated, but 9200 is protected.

Same positions, same prices. Cross: both survive. Isolated: both liquidated but free balance safe.

### Adding/removing margin to isolated positions

Users can top up or reduce isolated margin without closing:

Position approaching liquidation (allocated 600, unrealized \-500)

User: "Add 400 USDC to this position"

  allocated\_margin: 600 → 1000

  free\_balance: 9200 → 8800

  Liquidation price moves further away.

Removing margin: only allowed if it doesn't breach IM for that position.

### Unrealized PnL and mark price

Unrealized PnL changes every mark price tick. Not triggered by trades — triggered by the Mark Price Engine.

Timeline for alice (long 2 BTC @ 50000):

  t=0:  mark=50000  unrealized=0      equity=5000

  t=1:  mark=50500  unrealized=+1000  equity=6000

  t=2:  mark=49800  unrealized=-400   equity=4600

  t=3:  mark=49500  unrealized=-1000  equity=4000

No explicit "mark-to-market event." It's continuous. TradFi does this once daily (variation margin). We do it every tick.

### Liquidation price estimation

Users need to know where they get liquidated. Shown in UI and execution reports.

**Cross-margin (single position, simplified):**

Long:  liq\_price ≈ entry \- (equity \- MM\_other\_positions) / size

Short: liq\_price ≈ entry \+ (equity \- MM\_other\_positions) / size

**Isolated:**

Long:  liq\_price ≈ entry \- (allocated\_margin \- MM) / size

Short: liq\_price ≈ entry \+ (allocated\_margin \- MM) / size

Liquidation price changes on: mark price moves, deposits/withdrawals, other position changes (cross), margin additions (isolated), fee deductions. Pushed to user via WebSocket on every material change.

### Open interest tracking

Both parties opening new positions: OI \+= trade quantity

One opening, one closing:           OI unchanged

Both closing:                       OI \-= trade quantity

Published in market data (ticker stream). Used for position limits and risk monitoring.

### System diagram

┌──────────────────────────────────────────────────────────────┐

│                    CONTINUOUS RISK LOOP                        │

│                                                               │

│  ┌──────────────┐     ┌────────────────┐                     │

│  │  Mark Price   │────►│  Risk Engine    │                    │

│  │  Engine       │     │                 │                    │

│  │  (every tick) │     │  For each user: │                    │

│  └──────────────┘     │  1\. unrealized   │                    │

│         ▲              │     PnL          │                    │

│         │              │  2\. equity       │                    │

│  ┌──────────────┐     │  3\. vs MM        │                    │

│  │  Oracle      │     │  4\. decide       │                    │

│  │  (Pyth etc)  │     └───────┬─────────┘                    │

│  └──────────────┘             │                               │

│                          ┌────┴────┐                          │

│                       SAFE      BREACH                        │

│                          │         │                          │

│                          ▼         ▼                          │

│                     Continue   Liquidation Engine (Flow 5\)    │

│                                                               │

│  Push to user via WebSocket:                                  │

│  • unrealized PnL  • equity  • margin ratio                  │

│  • liquidation price  • margin warning (if approaching)      │

└──────────────────────────────────────────────────────────────┘

### Decisions Made — Flow 4

| \# | Decision | Choice | Rationale |
| :---- | :---- | :---- | :---- |
| 1 | Margin check frequency | Every mark price tick (sub-second) | Crypto moves fast. Daily checks insufficient. |
| 2 | Margin call | None — automatic liquidation | 24/7, pseudonymous users. No time for phone calls. |
| 3 | Unrealized PnL | Continuous, mark-price-driven | No daily settlement cycle. Always current. |
| 4 | Isolated margin management | User can add/remove margin per position | Standard UX (Binance, Bybit, OKX). |
| 5 | Liquidation price | Calculated and pushed on every material change | Via WebSocket. Essential for user risk management. |
| 6 | Open interest | Tracked per instrument, published in market data | Position limits \+ risk monitoring. |
| 7 | Position model | One position per user per instrument per margin mode | Cross long \+ isolated short on same instrument \= two separate positions. |

### Open Questions — Flow 4

- [ ] Position data push frequency — every tick or throttled (e.g., 100ms)?  
- [ ] Margin warning threshold — 1.1x IM? 1.2x IM?  
- [ ] Cross-margin liq price display with multiple positions — per-position estimate or account-level?

---

## Flow 5: Liquidation

### Overview

When a user's margin runs out, the engine must close their position before it goes bankrupt. This is the most critical safety system. Get it wrong and you get OKEx 2018: 17.7% clawback on all profitable traders.

### When liquidation triggers

Cross-margin:

  equity \= wallet\_balance \+ Σ unrealized\_pnl (all cross positions)

  total\_MM \= Σ (MMF(notional) × notional)

  if equity \<= total\_MM → LIQUIDATE

Isolated-margin:

  effective\_equity \= allocated\_margin \+ unrealized\_pnl

  position\_MM \= MMF(notional) × notional

  if effective\_equity \<= position\_MM → LIQUIDATE this position

### Liquidation price vs bankruptcy price

  Liquidation price: equity \= MM. Engine starts closing.

  Bankruptcy price:  equity \= 0\. Insurance vault needed.

  The gap between them is the buffer the exchange has to close

  the position without loss. Wider gap \= safer.

### The full process

TRIGGER: equity \<= maintenance\_margin

    │

    ▼

Step 1: CANCEL ALL OPEN ORDERS

    Cross-margin: cancel ALL orders across ALL instruments for this user.

      (All orders share equity — freeing margin on any instrument helps.)

    Isolated-margin: cancel orders only for the specific instrument being liquidated.

      (Isolated positions are ring-fenced — other instruments' orders are unaffected.)

    Free open\_order\_margin. May restore margin without any liquidation.

    │

    ▼

Step 2: RE-CHECK MARGIN

    Restored? → STOP. No liquidation needed.

    Still breached? → continue.

    │

    ▼

Step 3: SELECT POSITION(S) TO LIQUIDATE

    Isolated: that specific position.

    Cross (single): that position.

    Cross (multiple): largest unrealized loss first.

    │

    ▼

Step 4: GRADUAL LIQUIDATION LOOP

    │

    ├── chunk \= 10% of position size (min 1 lot)

    ├── place REDUCE-ONLY IOC on orderbook (with price band limit)

    ├── wait for fill (up to 1 second)

    ├── deduct 1% liquidation fee → insurance vault

    ├── re-check equity vs MM

    │     restored? → STOP (position survives, reduced)

    │     not restored? → next chunk

    │     position fully closed? → check equity

    │

    ▼

Step 5: SETTLEMENT

    │

    ├── equity \>= 0: Normal. User keeps remaining balance.

    │

    └── equity \< 0: BANKRUPT.

          deficit \= abs(equity)

          insurance vault absorbs deficit

          user wallet\_balance \= 0

          │

          └── vault depleted? → ADL (Step 6\)

### Why cancel orders first

A user with many resting orders has open\_order\_margin reserved. Cancelling them may free enough margin to avoid liquidation entirely.

### Why gradual liquidation

Full liquidation:   Close entire 10 BTC position. Hammers orderbook. Cascading liquidations.

Gradual (ours):     Close 1 BTC. Re-check. Maybe 9 BTC survives. Minimal market impact.

Adopted from Backpack: \~10% per iteration, minimum needed to restore margin health.

### Liquidation order pricing

Long being liquidated (selling):  limit \= mark\_price × (1 \- liquidation\_band%)

Short being liquidated (buying):  limit \= mark\_price × (1 \+ liquidation\_band%)

IOC with price band prevents fills at extreme prices. Unfilled portions go to next iteration.

### Cross-margin: position selection

Multiple cross positions → liquidate largest unrealized loss first. That position drags equity down most. Closing it (even partially) has the most impact on restoring margin.

### ADL (Auto-Deleveraging) — absolute last resort

Triggers when insurance vault is depleted and a bankrupt liquidation creates an uncovered deficit.

**Ranking:** Score \= PnL% × Effective Leverage (highest deleveraged first)

PnL% \= unrealized\_pnl / margin

Effective Leverage \= notional / equity

Only traders who are (a) profitable AND (b) on the OPPOSITE side of the

bankrupt position AND (c) in the SAME instrument are eligible for ADL.

Among eligible traders, highest score \= deleveraged first.

They're most able to absorb it and most contributing to systemic risk.

**ADL indicator (5 lights):** Shown to every user so they know their priority ranking.

■ ■ ■ ■ ■  \= highest priority (deleveraged first)

■ □ □ □ □  \= lowest priority (safe)

### How TradFi compares

|  | TradFi (CME) | Asgard |
| :---- | :---- | :---- |
| Trigger | Margin call, 12+ hours to respond | Automatic, instant |
| Who liquidates | Clearing member manages risk. CCP steps in on member default. | Engine liquidates automatically. |
| Loss waterfall | Defaulter margin → DF → CCP SITG → mutualized DF → assessments | Orderbook → insurance vault → ADL |

### System diagram

Risk Engine: equity \<= MM

    │

    ▼

Cancel all orders → margin freed → re-check

    │

    │ still breached

    ▼

Select position (largest loss) → gradual liquidation loop

    │                               │

    │                    ┌──────────┴──────────┐

    │                 restored              position gone

    │                    │                      │

    │                  STOP              ┌──────┴──────┐

    │                (survives)       equity\>=0     equity\<0

    │                                    │          (BANKRUPT)

    │                                  Done          │

    │                                         Insurance Vault

    │                                                │

    │                                         ┌──────┴──────┐

    │                                      vault OK     DEPLETED

    │                                         │              │

    │                                       Done           ADL

    │                                              (force-close profitable

    │                                               traders, ranked by

    │                                               PnL% × leverage)

### Decisions Made — Flow 5

| \# | Decision | Choice | Rationale |
| :---- | :---- | :---- | :---- |
| 1 | Liquidation trigger | equity \<= MM, continuous | Standard across all crypto exchanges. |
| 2 | First action | Cancel all open orders (cross: all instruments; isolated: that instrument only) | Frees margin. May avoid liquidation entirely. |
| 3 | Position selection (cross) | Largest unrealized loss first | Most impact on restoring margin. |
| 4 | Liquidation style | Gradual \~10% chunks, re-check after each | Adopted from Backpack. Minimum needed. Less market impact. |
| 5 | Liquidation order | Reduce-only IOC with price band | No resting. Price band prevents extreme fills. |
| 6 | Liquidation fee | 1% → insurance vault | Funds the vault. Calibrate with MM level. |
| 7 | Bankrupt handling | Insurance vault absorbs deficit. User balance \= 0\. | User never owes the exchange. |
| 8 | ADL trigger | Insurance vault depleted | Absolute last resort. |
| 9 | ADL ranking | PnL% × effective leverage, highest first | Industry standard (Binance, Bybit, Backpack). |
| 10 | ADL indicator | 5-level visual, shown to all users | Transparency. Users know their priority. |

### Open Questions — Flow 5

- [ ] Fee vs MM calibration — ensure fee doesn't guarantee bankruptcy for small positions  
- [ ] Liquidation chunk size — 10% default, configurable per instrument?  
- [ ] Price band width for liquidation orders — 5% from mark?  
- [ ] ADL execution price — mark, bankruptcy, or midpoint?  
- [ ] Cross-margin multi-position — after worst position, move to second-worst?

---




## Flow 7: Market Data Distribution

Every previous flow — order placement, matching, liquidation, settlement — ends with "broadcast." This flow is about how that broadcast actually works. What data gets published, to whom, through what infrastructure, and what guarantees the system provides about ordering and latency.

Market data is the product surface. A trader's entire experience — the orderbook they see, the price chart, the ticker, the fill notifications — is market data. An exchange with a fast matching engine but slow or unreliable market data distribution is, from the trader's perspective, a slow exchange.

### What data exists

The exchange produces two categories of market data: public (visible to everyone) and private (visible only to the account owner).

**Public data — generated by the trading engine:**

| Stream | Source | What triggers an update | Content |
| :---- | :---- | :---- | :---- |
| **Orderbook depth** | Matching engine | Any order placed, cancelled, modified, or filled | Price levels with aggregate quantity per side. No user identities. |
| **Trades** | Matching engine | Every fill | Price, quantity, taker side, trade ID, timestamp. No user identities. |
| **Ticker** | Market data engine (aggregates) | On trade, or periodic (1s) if no trades | Last price, 24h high/low, 24h volume (base \+ quote), price change %, best bid/ask. |
| **Best bid/offer (BBO)** | Matching engine | Any change to top-of-book | Best bid price/qty, best ask price/qty. Fastest-changing stream. |
| **Mark price** | Mark price engine | Every mark price recalculation | Mark price, index price, basis. Update frequency \= engine tick rate. |
| **Klines (candlesticks)** | Market data engine (aggregates) | On trade (updates open candle) \+ on interval close | OHLCV per interval. Intervals: 1m, 5m, 15m, 1h, 4h, 1d. |
| **Open interest** | Internal ledger | On position open/close/liquidation/settlement | Total OI in contracts and notional, per instrument. |
| **Liquidations** | Liquidation engine | On each liquidation fill | Instrument, side, quantity, price. No user identity. |

**Private data — per-user, authenticated:**

| Stream | Source | What triggers an update | Content |
| :---- | :---- | :---- | :---- |
| **Order updates** | Matching engine \+ OMS | Order placed, filled, partially filled, cancelled, rejected, expired, modified | Full order state: order ID, status, filled qty, avg fill price, fee, timestamp. |
| **Position updates** | Risk engine | On fill, liquidation, settlement, or margin recalculation | Position side, size, entry price, unrealized PnL, margin, liquidation price estimate. |
| **Balance updates** | Internal ledger | On fill (fee), deposit, withdrawal, settlement, liquidation | Wallet balance, equity, available balance, margin used. |
| **Margin warnings** | Risk engine | Equity approaches maintenance margin (equity ≤ IM × 1.1) | Warning level, current equity, required margin. |

### Two delivery mechanisms: WebSocket and REST

**WebSocket (primary, streaming):** Real-time push. Client opens a persistent connection, subscribes to channels, receives updates as they happen. This is how every active trader and every MM consumes data.

**REST (secondary, polling):** Request-response. Client asks for a snapshot and gets it. Used for: initial page load, recovery after disconnect, historical data queries, low-frequency integrations.

The two mechanisms are complementary, not alternatives. A typical client session:

1. Connect WebSocket  
2. Fetch REST snapshot (orderbook, positions, balances)  
3. Apply WebSocket deltas on top of the snapshot  
4. If WebSocket drops: reconnect, re-fetch snapshot, resume

### WebSocket channel design

Public channels:

  depth.\<instrument\>             Orderbook updates

  trade.\<instrument\>             Trade executions

  ticker.\<instrument\>            Ticker (aggregated)

  bbo.\<instrument\>               Best bid/offer only

  markPrice.\<instrument\>         Mark price \+ index

  kline.\<interval\>.\<instrument\>  Candlesticks (1m, 5m, 15m, 1h, 4h, 1d)

  openInterest.\<instrument\>      Open interest

  liquidation.\<instrument\>       Liquidation events

Private channels (authenticated):

  account.orders                 All order updates, all instruments

  account.orders.\<instrument\>    Order updates, one instrument

  account.positions              Position updates

  account.balances               Balance updates

  account.warnings               Margin warnings, expiry warnings

**Subscription format:**

{

  "method": "SUBSCRIBE",

  "params": \["depth.BTC-USDC-20260327", "trade.BTC-USDC-20260327"\]

}

{

  "method": "UNSUBSCRIBE",

  "params": \["depth.BTC-USDC-20260327"\]

}

Private channels require ED25519-signed authentication on the WebSocket connection (same key pair as REST API). Authenticate once per connection, then subscribe to private channels.

### Orderbook distribution: snapshots and deltas

This is the hardest part of market data. The orderbook changes on every order, cancel, and fill. A busy instrument might produce hundreds of updates per second. Sending the full orderbook on every change is wasteful. Sending only deltas is efficient but creates a consistency problem: if a client misses one delta, their local book is permanently wrong.

**The solution: sequence numbers \+ periodic snapshots.**

Every orderbook update carries a monotonically increasing sequence number from the matching engine. The client tracks the last sequence number it processed. If it detects a gap, it knows it missed an update and must re-sync.

┌──────────────────────────────────────────────────────────────────┐

│                     Orderbook Distribution                       │

│                                                                  │

│  Matching Engine                                                 │

│       │                                                          │

│       │ (every book change)                                      │

│       ▼                                                          │

│  ┌─────────────────┐                                             │

│  │ Market Data      │                                            │

│  │ Engine           │                                            │

│  │                  │──── delta update ──► WebSocket fanout       │

│  │  \- seq tracking  │         │           to all subscribers      │

│  │  \- aggregation   │         │                                   │

│  │  \- snapshot gen  │         │                                   │

│  └─────────────────┘         │                                   │

│       │                      │                                   │

│       │ (every N seconds     │                                   │

│       │  or on request)      │                                   │

│       ▼                      │                                   │

│  Full snapshot               │                                   │

│  (REST: GET /depth)          │                                   │

│                              │                                   │

│  Client state machine:       │                                   │

│  ┌─────────────────────────────────────────────┐                 │

│  │ 1\. GET /depth → full snapshot (seq=1000)    │                 │

│  │ 2\. Subscribe depth.\<instrument\>             │                 │

│  │ 3\. Buffer deltas, discard any with seq≤1000 │                 │

│  │ 4\. Apply deltas sequentially from seq=1001  │                 │

│  │ 5\. If gap detected: go to step 1            │                 │

│  └─────────────────────────────────────────────┘                 │

└──────────────────────────────────────────────────────────────────┘

**Delta format:**

{

  "stream": "depth.BTC-USDC-20260327",

  "data": {

    "bids": \[\[50000, 2\], \[49900, 0\]\],

    "asks": \[\[50100, 3\]\],

    "seq": 1274,

    "timestamp": 1711036800123456

  }

}

Each entry is `[price, quantity]`. Quantity of 0 means remove that price level. This is the standard L2 incremental update format used by Binance, Backpack, dYdX, and most production exchanges.

**Depth levels for REST snapshots:**

`GET /api/v1/depth?instrument=BTC-USDC-20260327&limit=100`

Available limits: 5, 10, 20, 50, 100, 500, 1000\. Default: 100\.

5 and 10 are for mobile UIs and lightweight widgets. 100 is the typical trading UI. 500+ is for MMs and algo traders who need to see deep into the book.

### L2 vs L3 orderbook data

**L2 (aggregate by price level):** Each price level shows total quantity. This is what the WebSocket `depth` stream and REST `/depth` endpoint provide. Every retail and most institutional clients use L2.

**L3 (individual orders):** Each order is visible separately — order ID, price, quantity. Used by sophisticated MMs and surveillance systems. Reveals queue position (how deep your order is at a given price level). L3 is more expensive to produce, transmit, and process.

**MVP: L2 only.** L3 is a V1.1 feature. Reasons:

- L2 covers 95%+ of use cases including MM quoting  
- L3 data volume is an order of magnitude larger  
- L3 leaks information about individual order sizes (possible front-running vector)  
- No crypto exchange except FTX (defunct) offered L3 publicly. Deribit and Binance are L2 only. Backpack is L2.

### Ticker construction

The ticker is a rolling summary. Unlike depth and trade streams that report raw events, the ticker is computed by aggregating.

Ticker fields:

  instrument         "BTC-USDC-20260327"

  last\_price         50100                    last trade price

  best\_bid           50000                    current top of bid book

  best\_ask           50100                    current top of ask book

  price\_change\_24h   \+300                     last\_price \- price\_24h\_ago

  price\_change\_pct   \+0.60%                   percentage change

  high\_24h           50500                    highest trade in 24h

  low\_24h            49700                    lowest trade in 24h

  volume\_24h         1250                     base volume (contracts)

  quote\_volume\_24h   62,500,000               quote volume (USDC notional)

  open\_interest      8500                     current OI (contracts)

  mark\_price         50050                    current mark price

  index\_price        50020                    current oracle index

  timestamp          1711036800123456         microseconds

**Update frequency:** On every trade. If no trades for 1 second, send the current state anyway (so clients know the stream is alive). This 1-second heartbeat also updates mark price and BBO changes that happened without a trade.

**REST equivalent:** `GET /api/v1/ticker?instrument=BTC-USDC-20260327`

### Kline (candlestick) construction

Klines are OHLCV (open, high, low, close, volume) bars aggregated over time intervals.

Kline fields:

  instrument         "BTC-USDC-20260327"

  interval           "1m"

  open\_time          1711036800000000         interval start (microseconds)

  close\_time         1711036859999999         interval end (microseconds)

  open               50000

  high               50200

  low                49950

  close              50100

  volume             42                       base volume (contracts)

  quote\_volume       2,100,000                quote volume (USDC)

  trades             18                       number of trades in interval

**Supported intervals:** 1m, 5m, 15m, 1h, 4h, 1d.

**WebSocket behavior:** The current (unclosed) candle updates on every trade. When the interval closes, a final update is sent with `closed: true` and the next candle opens.

**REST:** `GET /api/v1/klines?instrument=BTC-USDC-20260327&interval=1h&limit=500`

Returns historical closed candles plus the current open candle. Max 1500 candles per request. This is what charting libraries (TradingView, lightweight-charts) consume on initial load.

**Storage:** Klines are materialized — the market data engine computes and stores them, not computed on-the-fly from trade history. 1m candles are the base. Larger intervals (5m, 15m, etc.) can be rolled up from 1m candles for historical queries, but the live stream computes each interval independently from the trade feed for lowest latency.

### Mark price and index price distribution

Mark price is not market data in the traditional sense — it's a derived price used for margin calculation, liquidation triggers, and stop order evaluation. But it's distributed through the same infrastructure because traders need to see it.

Mark price update:

{

  "stream": "markPrice.BTC-USDC-20260327",

  "data": {

    "mark\_price": 50050,

    "index\_price": 50020,

    "basis": 30,

    "basis\_pct": 0.06,

    "timestamp": 1711036800123456

  }

}

**Update frequency:** Every mark price engine tick. The mark price engine runs on every oracle update and every change to the orderbook mid-price (since mark \= index \+ EWMA(mid \- index)). In practice, this means multiple updates per second.

**Why distribute basis?** Basis (futures price \- spot index) is the core metric for futures traders. Positive basis \= contango (futures above spot). Negative \= backwardation. Distributing it pre-computed saves every client from calculating it themselves and ensures consistency.

### Open interest distribution

Open interest update:

{

  "stream": "openInterest.BTC-USDC-20260327",

  "data": {

    "open\_interest": 8500,

    "open\_interest\_notional": 425000000,

    "timestamp": 1711036800123456

  }

}

**Update frequency:** On every position open, close, liquidation, or settlement that changes OI. Not on every trade — a trade between two existing position holders (one reducing, one increasing by the same amount) doesn't change OI. Batched to at most 1 update per second to avoid spam during high-activity periods.

**REST:** `GET /api/v1/openInterest?instrument=BTC-USDC-20260327`

### Liquidation feed

Liquidation event:

{

  "stream": "liquidation.BTC-USDC-20260327",

  "data": {

    "side": "long",

    "quantity": 1,

    "price": 48500,

    "timestamp": 1711036800123456

  }

}

No user identity. Only the side, size, and price of the liquidation fill. This tells the market that forced selling/buying is happening — useful context for all traders.

**Why publish liquidations?** Transparency. Cascading liquidations are a systemic risk in leveraged markets. Traders and risk monitors need to see them in real time. Backpack, Binance, and Deribit all publish liquidation feeds.

### Infrastructure: how updates reach clients

                    ┌─────────────────────────────┐

                    │     Internal Event Bus       │

                    │   (matching engine output)    │

                    └──────────┬──────────────────┘

                               │

               ┌───────────────┼───────────────┐

               │               │               │

               ▼               ▼               ▼

        ┌────────────┐  ┌────────────┐  ┌────────────┐

        │ Market Data │  │ Risk       │  │ Audit      │

        │ Engine      │  │ Engine     │  │ Trail      │

        │             │  │            │  │            │

        │ \- depth agg │  │ (Flow 4\)   │  │ (persist)  │

        │ \- ticker    │  │            │  │            │

        │ \- klines    │  │            │  │            │

        │ \- OI track  │  │            │  │            │

        └──────┬─────┘  └────────────┘  └────────────┘

               │

               │  (pub/sub topics per stream)

               ▼

        ┌────────────────────────┐

        │  WebSocket Fanout Layer │

        │                        │

        │  Per-topic subscriber  │

        │  lists. Each WS node   │

        │  subscribes to topics  │

        │  its clients need.     │

        └──────┬─────────────────┘

               │

       ┌───────┼───────┐

       │       │       │

       ▼       ▼       ▼

    ┌─────┐ ┌─────┐ ┌─────┐

    │ WS  │ │ WS  │ │ WS  │    N WebSocket server nodes

    │ Pod │ │ Pod │ │ Pod │    (horizontally scalable)

    │  1  │ │  2  │ │  3  │

    └──┬──┘ └──┬──┘ └──┬──┘

       │       │       │

       ▼       ▼       ▼

    clients  clients  clients

**The key constraint: the matching engine is single-threaded.** It produces events in a deterministic sequence. The market data engine consumes these events and fans out to subscribers. The matching engine must never block on market data distribution — it fires events into the bus and moves on.

**WebSocket fanout layer:** This is the horizontal scaling point. The matching engine doesn't know or care how many clients are connected. It publishes events to topics. WebSocket pods subscribe to the topics their connected clients need. Adding more WS pods \= supporting more concurrent connections without touching the trading engine.

**Topic-based routing:** Each stream (e.g., `depth.BTC-USDC-20260327`) is a topic. A WS pod with 100 clients subscribed to BTC depth receives one copy of each depth update and fans it out to all 100 clients. This is dramatically more efficient than the matching engine sending 100 individual messages.

**Internal event bus options (TBD):**

- In-process channels (if market data engine is co-located with matching engine) — lowest latency  
- Redis pub/sub — simple, proven, \~0.1ms overhead  
- NATS — purpose-built pub/sub, supports subject-based routing, clustering  
- Kafka — if we need replay and persistence (overkill for real-time fanout, useful for kline construction and audit)

The choice depends on deployment topology. If matching engine and market data engine run in the same process, in-process channels with an external pub/sub for WS fanout is the simplest. If they're separate services, the bus needs to be external.

### Timestamp precision

All WebSocket timestamps are in **microseconds** (not milliseconds). This matches Backpack's convention and provides the precision MMs need for latency measurement and sequencing.

REST API timestamps are also in microseconds for consistency, though REST consumers rarely need sub-millisecond precision.

### Ordering guarantees

**Within a single stream:** Updates are delivered in sequence number order. If a client receives seq 1274, the next update on that stream will be seq 1275 or higher. Gaps mean missed messages → re-sync.

**Across streams:** No global ordering guarantee. A trade update and its corresponding depth update may arrive in either order. This is standard — Binance, Backpack, and every high-performance exchange works this way. Global ordering across streams would require serialization, which kills throughput.

**Private vs public:** A user's fill notification (private) may arrive before or after the corresponding public trade event. In practice, the private event is usually faster (shorter path — doesn't go through aggregation). Clients should not assume ordering between private and public streams.

### Backpressure and slow consumers

A slow client that can't process messages fast enough will cause its send buffer to grow. If unchecked, this consumes server memory and eventually crashes the WS pod.

**Strategy: bounded buffer \+ disconnect.**

Each WebSocket connection has a bounded send buffer (e.g., 1000 messages or 1MB). If the buffer fills:

1. Skip intermediate orderbook updates — send only the latest state (coalesce)  
2. If still overflowing — disconnect the client with a "slow consumer" error code  
3. Client reconnects, re-fetches snapshot, resumes

This is harsh but necessary. The alternative — unbounded buffering — means one slow client degrades the entire WS pod. Binance and Deribit both disconnect slow consumers.

**MM-specific consideration:** MMs generate the most traffic (mass\_quote updates) and also consume the most market data. They are the most likely to be fast consumers. But their connections should still be subject to the same backpressure rules — no special treatment that could create a shared resource risk.

### Rate limits on WebSocket subscriptions

**Per-connection subscription limit:** Max 100 streams per connection. This prevents a single client from subscribing to every instrument × every stream type and creating a fan-out explosion.

**Connection limit per user:** Max 5 WebSocket connections per API key. MMs who need more streams use multiple connections with different subscription sets.

**Message rate on private streams:** Not rate-limited outbound (the exchange pushes as fast as events happen). But the data itself is bounded — a user can only have so many active orders and positions.

### REST market data endpoints

| Endpoint | Description | Auth | Cache |
| :---- | :---- | :---- | :---- |
| `GET /api/v1/depth` | Orderbook snapshot | No | No (real-time) |
| `GET /api/v1/trades` | Recent trades (last N) | No | No |
| `GET /api/v1/trades/history` | Historical trades (paginated) | No | Yes (immutable data) |
| `GET /api/v1/ticker` | Single instrument ticker | No | 1s |
| `GET /api/v1/tickers` | All instruments tickers | No | 1s |
| `GET /api/v1/klines` | Historical \+ current klines | No | Closed candles: long. Open candle: 1s. |
| `GET /api/v1/markPrices` | Mark \+ index prices, all instruments | No | 1s |
| `GET /api/v1/openInterest` | OI per instrument | No | 1s |

**Caching strategy:** Immutable data (closed klines, historical trades) gets aggressive caching. Real-time data (depth, current candle) gets no or very short cache. Tickers and mark prices get 1-second cache — frequent enough for display, prevents thundering herd on page loads.

REST rate limits (TBD per tier) apply to all public endpoints. Market data endpoints are the most-called endpoints on any exchange — they need CDN-level caching or they'll overwhelm the API layer on traffic spikes.

### What happens during an example trading session

Alice opens the Asgard trading UI for BTC-USDC-20260327. Here's the exact data flow:

1\. Page load:

   REST: GET /api/v1/depth?instrument=BTC-USDC-20260327\&limit=100

   REST: GET /api/v1/klines?instrument=BTC-USDC-20260327\&interval=1m\&limit=500

   REST: GET /api/v1/ticker?instrument=BTC-USDC-20260327

   REST: GET /api/v1/markPrices

   → UI renders: orderbook, chart, ticker bar, mark price

2\. WebSocket connect \+ authenticate:

   → Subscribe: depth.BTC-USDC-20260327

                trade.BTC-USDC-20260327

                ticker.BTC-USDC-20260327

                markPrice.BTC-USDC-20260327

                account.orders.BTC-USDC-20260327

                account.positions

                account.balances

3\. Live updates flow:

   Someone places a limit order → depth update arrives → orderbook UI updates

   A trade executes → trade event \+ depth update \+ ticker update arrive

   Mark price ticks → markPrice update → PnL display recalculates

   Alice places an order → account.orders: { status: "open" }

   Alice's order fills → account.orders: { status: "filled" }

                        \+ account.positions: { new position }

                        \+ account.balances: { fee deducted }

                        \+ (public) trade event \+ depth update \+ ticker update

4\. Alice disconnects (closes tab):

   WebSocket closes → server cleans up subscriptions

   No state to persist server-side (subscriptions are ephemeral)

### How TradFi compares

|  | TradFi (CME/NSE) | Asgard |
| :---- | :---- | :---- |
| **Delivery protocol** | Dedicated multicast feeds (CME MDP 3.0, NSE EMDI) | WebSocket \+ REST |
| **Orderbook data** | L2 aggregate \+ L3 individual orders (CME Market by Order) | L2 aggregate only (MVP). L3 in V1.1. |
| **Update model** | Incremental with periodic snapshots (identical concept) | Same — delta \+ seq \+ snapshot recovery |
| **Latency** | \~1-5 μs (co-location) to \~1-10 ms (remote) | \~1-10 ms (WebSocket over internet) |
| **Co-location** | Yes — rack space in exchange datacenter, direct feed | Not MVP. Possible V2 (dedicated connections for MMs). |
| **Market data fees** | Expensive. CME charges $1000+/mo for real-time L2. | Free. Revenue is from trading fees, not data fees. |
| **FIX protocol** | Standard for institutional connectivity | V1.1 — FIX gateway for MMs who require it. |

The fundamental model is identical: producer (matching engine) → aggregator (market data engine) → fan-out (distribution infrastructure) → consumers. TradFi uses multicast UDP and dedicated network hardware. We use WebSocket over TCP. The tradeoff: TradFi gets microsecond latency for co-located firms. We get universal internet accessibility with millisecond latency.

### System Diagram — Market Data Distribution

┌───────────────────────────────────────────────────────────────────────────┐

│                                                                           │

│  ┌─────────────┐                                                          │

│  │  Matching    │──── order/trade/cancel events ──────┐                   │

│  │  Engine      │                                     │                   │

│  └─────────────┘                                      │                   │

│                                                       ▼                   │

│  ┌─────────────┐                            ┌──────────────────┐          │

│  │  Mark Price  │── mark/index updates ────►│  Market Data      │          │

│  │  Engine      │                            │  Engine           │          │

│  └─────────────┘                            │                   │          │

│                                              │  Produces:        │          │

│  ┌─────────────┐                            │  \- depth deltas   │          │

│  │  Liquidation │── liquidation events ────►│  \- trade events   │          │

│  │  Engine      │                            │  \- ticker agg     │          │

│  └─────────────┘                            │  \- kline agg      │          │

│                                              │  \- OI tracking    │          │

│  ┌─────────────┐                            │  \- BBO            │          │

│  │  Risk Engine │── margin warnings ───────►│  \- liq events     │          │

│  │              │   position updates         │  \- snapshots      │          │

│  └─────────────┘   (private, per-user)      └────────┬─────────┘          │

│                                                       │                   │

│                              ┌─────────────────────────┤                   │

│                              │                         │                   │

│                         public topics            private topics            │

│                              │                         │                   │

│                              ▼                         ▼                   │

│                     ┌──────────────────────────────────────┐               │

│                     │         Event Bus / Pub-Sub           │               │

│                     │   (topics: depth.X, trade.X,          │               │

│                     │    account.orders.user123, etc.)       │               │

│                     └────────────────┬─────────────────────┘               │

│                                      │                                     │

│                          ┌───────────┼───────────┐                         │

│                          │           │           │                         │

│                          ▼           ▼           ▼                         │

│                      ┌───────┐   ┌───────┐   ┌───────┐                    │

│                      │ WS    │   │ WS    │   │ WS    │                    │

│                      │ Pod 1 │   │ Pod 2 │   │ Pod 3 │                    │

│                      └───┬───┘   └───┬───┘   └───┬───┘                    │

│                          │           │           │                         │

│                       clients     clients     clients                      │

│                                                                           │

│                     ┌──────────────────────────────────────┐               │

│                     │         REST API Layer                │               │

│                     │   /depth  /trades  /ticker  /klines   │               │

│                     │   (reads from market data engine      │               │

│                     │    snapshots \+ materialized stores)    │               │

│                     └──────────────────────────────────────┘               │

└───────────────────────────────────────────────────────────────────────────┘

### Decisions Made — Flow 7

| \# | Decision | Choice | Rationale |
| :---- | :---- | :---- | :---- |
| 1 | Orderbook distribution model | L2 incremental deltas \+ sequence numbers \+ snapshot recovery | Industry standard. Efficient bandwidth. Proven at scale (Binance, Backpack, dYdX). |
| 2 | Orderbook depth level | L2 only for MVP. L3 in V1.1. | L2 covers 95%+ of use cases. L3 is expensive, leaks order-level info, no crypto exchange offers it publicly. |
| 3 | Timestamp precision | Microseconds throughout | Matches Backpack convention. MMs need sub-ms precision for latency measurement. |
| 4 | Slow consumer handling | Bounded buffer → coalesce → disconnect | Protects WS pod stability. Industry standard (Binance, Deribit disconnect slow consumers). |
| 5 | Ticker update frequency | On every trade \+ 1-second heartbeat if no trades | Keeps stream alive, ensures mark/BBO changes propagate even without trades. |
| 6 | Kline intervals | 1m, 5m, 15m, 1h, 4h, 1d | Standard set. Covers charting needs. 1m is the base; larger intervals roll up. |
| 7 | Kline storage | Materialized (pre-computed and stored) | Avoids recomputing from trade history on every REST request. Standard approach. |
| 8 | Market data fees | Free | Revenue from trading fees, not data. Free data attracts traders and MMs. |
| 9 | WebSocket subscription limit | 100 streams per connection, 5 connections per API key | Prevents fan-out explosion from single client. MMs spread across connections. |
| 10 | Private stream ordering | No guarantee relative to public streams | Serializing across private/public would kill throughput. Standard tradeoff. |

### Open Questions — Flow 7

- [ ] Event bus technology — in-process channels, Redis pub/sub, NATS, or Kafka? Depends on deployment topology.  
- [ ] Orderbook snapshot frequency — should the market data engine publish periodic snapshots to WebSocket (e.g., every 60s) in addition to REST, to help slow clients recover without disconnecting?  
- [ ] Depth stream throttling — should depth updates be batched/throttled to N updates per second for very active books? Or always send every change? Binance batches at 100ms intervals. Backpack sends every change.  
- [ ] FIX protocol gateway — needed for MVP or V1.1? Some institutional MMs only speak FIX.  
- [ ] Co-location / dedicated feed — offer direct TCP market data feeds (not WebSocket) for MMs in V1.1?  
- [ ] Kline base interval — is 1m sufficient as the base, or should we store tick-level data and compute all intervals from that?  
- [ ] REST rate limits per tier — what limits for public market data endpoints? Needs to be generous enough for trading bots but protected against abuse.  
- [ ] WebSocket compression — use permessage-deflate? Reduces bandwidth \~60-70% but adds CPU cost per connection. Worth it at scale.

---

## Flow 8: Circuit Breakers & Price Bands

Flow 2 introduced price bands as a pre-trade check — reject orders that are obviously mispriced (fat finger protection). This flow goes deeper. Price bands are one layer of a multi-layer protection system that prevents the exchange from executing trades at prices that would cause systemic damage: cascading liquidations, insurance vault drain, or loss of market integrity.

The core problem: in a leveraged market, a single bad price can trigger a chain reaction. A large market sell pushes the price down → liquidations trigger → liquidation orders push the price further down → more liquidations → and so on until the insurance vault is depleted and ADL kicks in. This is not hypothetical — it has happened repeatedly. Hyperliquid's JELLY incident (March 2025), BitMEX's multiple flash crashes, OKX's 2018 clawback. Every time, the root cause was the same: the exchange allowed trades to execute at prices far from fair value, and the cascade destroyed capital.

Circuit breakers exist to interrupt this chain. They are the exchange's immune system — they sacrifice some trading continuity to prevent systemic collapse.

### Layer 1: Limit Price Bands (pre-trade, per order)

This is what Flow 2 described. Every limit order is checked against a band around the current mark price before it enters the book.

For buy orders:  price \<= mark\_price × (1 \+ band%)

For sell orders: price \>= mark\_price × (1 \- band%)

**Reference price:** Mark price (index \+ EWMA of (mid \- index), bounded ±5% of index). Using mark rather than last traded price makes this band resistant to manipulation — you'd have to move the oracle index to widen the band.

**Band width:** Per instrument, set by the Instrument Manager. Wider for volatile assets, tighter for stable ones.

Example bands (TBD, illustrative):

  BTC-USDC futures:  ±5%  of mark

  XAU-USDC futures:  ±3%  of mark

**What gets rejected:** The order itself. No state change, no market impact. The user gets an immediate rejection with a clear error: "Price outside limit band. Current mark: 50,000. Max buy: 52,500."

**Market orders bypass this check** — they have no price limit. They fill at whatever price is available on the opposite side of the book. This is why market orders need a different protection mechanism (Layer 2).

### Layer 2: Price Impact Bands (execution-time, per fill)

This protects against market orders and aggressive limit orders that would sweep through multiple price levels, executing at progressively worse prices.

For buy fills:   fill\_price \<= best\_ask\_at\_order\_arrival × (1 \+ impact\_band%)

For sell fills:  fill\_price \>= best\_bid\_at\_order\_arrival × (1 \- impact\_band%)

The reference here is the best available price at the moment the order entered the matching engine — not the mark price. This measures how far the order itself moves the market.

Example:

  Alice places market buy for 50 lots.

  Best ask when order arrives: 50,100

  Impact band: 2%

  Max fill price: 50,100 × 1.02 \= 51,102

  Book sweeps:

    50,100 × 10 lots  → filled ✓

    50,200 × 15 lots  → filled ✓

    50,500 × 10 lots  → filled ✓

    50,800 × 8 lots   → filled ✓

    51,200 × 20 lots  → STOPPED (51,200 \> 51,102)

  Result: 43 lots filled, 7 lots cancelled (unfilled remainder)

  Alice gets partial fill, not a catastrophic sweep to $55,000.

**Why not just reject the whole order?** Because stopping the sweep partway is better than rejecting it entirely. The trader wanted to buy. They got most of their fill at reasonable prices. Only the extreme tail was cut off.

**Implementation:** The matching engine checks each fill against the impact ceiling as it walks the book. When a fill would exceed the band, it stops matching. Remaining quantity is cancelled (for market and IOC orders) or rests at the impact ceiling price (for GTC limit orders, though this case is rare since the limit price itself would usually be the binding constraint).

### Layer 3: Velocity Detection (time-based, per instrument)

Layers 1 and 2 protect against individual orders. Layer 3 detects when the market itself is moving too fast — a series of individually-valid trades that collectively constitute a crash or spike.

Velocity trigger:

  If mark\_price moves \> velocity\_threshold% within velocity\_window seconds:

    → activate cooldown mode for this instrument

**Velocity parameters (per instrument, TBD):**

Example (illustrative):

  BTC-USDC futures:

    velocity\_threshold: 8%

    velocity\_window:    60 seconds

    cooldown\_duration:  30 seconds

  XAU-USDC futures:

    velocity\_threshold: 5%

    velocity\_window:    60 seconds

    cooldown\_duration:  30 seconds

**What happens during cooldown:**

The instrument enters a restricted state. Not a full halt — that would be too aggressive for a 24/7 market. Instead:

COOLDOWN mode:

  ✓ Existing orders remain in the book (not cancelled)

  ✓ New limit orders accepted (provides liquidity)

  ✓ Cancel orders accepted

  ✓ Reduce-only orders accepted (let people close positions)

  ✗ New market orders rejected

  ✗ New aggressive limit orders rejected (orders that would immediately cross)

  ✗ Stop orders do NOT trigger (frozen during cooldown)

  Effect: the book can rebuild. MMs can re-quote. No new aggression.

  After cooldown\_duration: instrument returns to normal TRADING state.

**Why not a full halt?** Two reasons.

First, crypto markets are 24/7 with no opening auction mechanism (yet). A halt means zero price discovery. In TradFi, a halt is followed by a volatility auction that reopens the market — CME and Eurex both do this. We don't have an auction engine in MVP. A halt without a reopening mechanism means someone has to manually flip a switch, which is operationally dangerous at 3 AM.

Second, the cooldown mode is strictly better. It lets the market heal while still allowing defensive actions (closing positions, adding liquidity). The restriction is on new aggression, not on all activity.

**CME's velocity logic works the same way conceptually.** CME detects rapid price movement within a short window and triggers a brief pause. The difference: CME follows the pause with a volatility auction (because they have an auction engine). We follow the cooldown with a timer-based return to normal trading. The auction is a V1.1 consideration.

**Detection implementation:**

The velocity detector maintains a rolling window of mark prices:

  price\_history \= ring\_buffer of (timestamp, mark\_price), window \= 60s

  On every mark price update:

    oldest \= price\_history.oldest()

    newest \= current mark price

    move \= abs(newest \- oldest) / oldest

    if move \> velocity\_threshold:

      instrument.state \= COOLDOWN

      cooldown\_expires\_at \= now() \+ cooldown\_duration

Simple, deterministic, no ML or heuristics. A ring buffer of prices and one comparison.

**Known limitation:** This compares only the oldest and newest prices in the window. A round-trip move (7% up then 7% back down within 60s) would show \~0% net movement and not trigger the cooldown, even though the market experienced extreme volatility. A more sophisticated approach would track the max-to-min range within the window. For MVP, the endpoint comparison is sufficient — real crashes are directional, not round-trips. A range-based detector is a V1.1 enhancement.

### Layer 4: Market-Wide Circuit Breaker

Layers 1–3 operate per instrument. Layer 4 is a system-wide emergency brake.

**When does it trigger?**

Conditions (any one triggers):

  1\. Multiple instruments in COOLDOWN simultaneously

     (configurable threshold — at launch with 2 instruments, "both in

      cooldown" \= systemic event. Scale threshold as instruments grow.)

  2\. Insurance vault utilization crosses emergency threshold (e.g., 90%)

  3\. Manual admin trigger (kill switch)

Condition 1 catches correlated crashes — if BTC, ETH, and XAU futures are all in velocity cooldown at the same time, something systemic is happening (macro event, oracle failure, coordinated attack).

Condition 2 catches the insurance vault approaching depletion regardless of cause.

Condition 3 is the human override for anything the automated system doesn't catch.

**What happens:**

EMERGENCY mode (all instruments):

  ✓ Reduce-only orders accepted (let people close)

  ✓ Cancel orders accepted

  ✓ Withdrawals continue (never block withdrawals — that's FTX behavior)

  ✗ All new position-opening orders rejected

  ✗ Stop orders frozen

  ✗ New deposits credited but not available for trading

  \+ Alert sent to all connected users via WebSocket

  \+ Alert sent to ops team (PagerDuty or equivalent)

  \+ All events logged with EMERGENCY severity

**Recovery:** Manual only. An admin must explicitly return the system to normal after assessing the situation. There is no automatic timer. The system errs on the side of staying in emergency mode until a human confirms it's safe.

**Why withdrawals stay open:** Blocking withdrawals during a crisis is the single worst thing an exchange can do for trust. FTX blocked withdrawals. That's the line we never cross. Users must always be able to leave. If the exchange is insolvent, blocking withdrawals doesn't fix insolvency — it just adds fraud to the list.

### How the layers interact

┌─────────────────────────────────────────────────────────────────────┐

│                    Protection Layers                                 │

│                                                                     │

│  Order arrives                                                      │

│       │                                                             │

│       ▼                                                             │

│  ┌──────────────────────────────────────┐                           │

│  │ Layer 1: Limit Price Band            │  Pre-trade check          │

│  │ Is order price within ±X% of mark?   │  per order                │

│  │                                      │                           │

│  │ REJECT if outside band               │                           │

│  └───────────────┬──────────────────────┘                           │

│                  │ pass                                              │

│                  ▼                                                   │

│  ┌──────────────────────────────────────┐                           │

│  │ Layer 2: Price Impact Band           │  Execution-time check     │

│  │ Would this fill move price \> Y%      │  per fill                 │

│  │ from best price at arrival?          │                           │

│  │                                      │                           │

│  │ STOP matching if impact exceeded     │                           │

│  └───────────────┬──────────────────────┘                           │

│                  │ fills within band                                 │

│                  ▼                                                   │

│           Trade executes                                            │

│                  │                                                   │

│                  ▼                                                   │

│  ┌──────────────────────────────────────┐                           │

│  │ Layer 3: Velocity Detection          │  Post-trade monitoring    │

│  │ Has mark moved \> Z% in last 60s?    │  continuous, per instrument│

│  │                                      │                           │

│  │ COOLDOWN if velocity exceeded        │                           │

│  └───────────────┬──────────────────────┘                           │

│                  │                                                   │

│                  ▼                                                   │

│  ┌──────────────────────────────────────┐                           │

│  │ Layer 4: Market-Wide Breaker         │  System-level monitoring  │

│  │ Multiple cooldowns? Vault stressed?  │  continuous, all markets  │

│  │                                      │                           │

│  │ EMERGENCY if systemic risk detected  │                           │

│  └──────────────────────────────────────┘                           │

│                                                                     │

│  Each layer catches what the previous layer missed.                 │

│  Layer 1: bad orders. Layer 2: bad sweeps. Layer 3: bad trends.     │

│  Layer 4: systemic events.                                          │

└─────────────────────────────────────────────────────────────────────┘

### Instrument states and transitions

Flow 2 introduced instrument states. Circuit breakers add two new states: COOLDOWN and EMERGENCY.

                    ┌──────────┐

                    │ PRE\_OPEN │

                    └────┬─────┘

                         │ listing time reached

                         ▼

                    ┌──────────┐

              ┌────►│ TRADING  │◄────────────────────┐

              │     └──┬───┬───┘                     │

              │        │   │                         │

              │        │   │ velocity threshold      │ cooldown expires

              │        │   │ exceeded                │

              │        │   ▼                         │

              │        │ ┌───────────┐               │

              │        │ │ COOLDOWN  │───────────────┘

              │        │ └─────┬─────┘

              │        │       │

              │        │       │ 3+ instruments in cooldown

              │        │       │ OR vault utilization \> 90%

              │        │       │ OR admin trigger

              │        ▼       ▼

              │     ┌─────────────┐

              │     │  EMERGENCY  │

              │     └──────┬──────┘

              │            │ admin manual recovery

              │            │

              └────────────┘

              │

              │ T-10min before expiry

              ▼

         ┌────────────┐

         │ CLOSE\_ONLY │

         └─────┬──────┘

               │ settlement window

               ▼

         ┌────────────┐

         │  SETTLING  │

         └─────┬──────┘

               │

               ▼

         ┌────────────┐

         │  SETTLED   │

         └────────────┘

**State transition rules:**

- TRADING → COOLDOWN: automatic (velocity detection)  
- COOLDOWN → TRADING: automatic (timer expires)  
- COOLDOWN → EMERGENCY: automatic (multiple cooldowns or vault stress) or manual  
- TRADING → EMERGENCY: manual (admin kill switch) or automatic (vault threshold)  
- EMERGENCY → TRADING: manual only (admin recovery)  
- COOLDOWN/EMERGENCY → CLOSE\_ONLY: expiry takes priority (settlement cannot be delayed by circuit breakers)

The last rule is important. If a contract is approaching expiry and the market is in COOLDOWN or EMERGENCY, the instrument still transitions to CLOSE\_ONLY at the scheduled time. Settlement cannot be postponed by circuit breakers — the contract expires when it expires. Users with positions must close or be settled. The exception is oracle failure during settlement, which is handled in Flow 9\.

### What Backpack does (for reference)

Backpack has four layers of price protection:

| Layer | Mechanism | How it works |
| :---- | :---- | :---- |
| **Limit Price Bands** | Pre-trade rejection | Orders outside Active Price × max/min multiplier rejected. SOL example: 2x max, 0.25x min. |
| **Price Impact Bands** | Execution-time stop | Taker fills restricted to best offer × max impact multiplier. Prevents book sweeps. |
| **Mean Mark Price Bands** | Post-fill expiry | Unfilled taker orders beyond 5-min mean mark price × multiplier are expired. SOL: ±3%. |
| **Mean Premium Bands** | Premium velocity | Tolerance-based restrictions on how fast the premium (futures \- spot) can change. |

Our Layer 1 and Layer 2 map directly to Backpack's first two. Our Layer 3 (velocity) is conceptually similar to their Mean Premium Bands but operates on mark price movement rather than premium specifically. Their Mean Mark Price Band (layer 3\) is an interesting mechanism — it catches stale taker orders that were valid when placed but became extreme by the time they might fill. Worth considering for V1.1.

### Liquidation interaction with circuit breakers

Circuit breakers and the liquidation engine have a complex relationship. Liquidation orders are force-sells/buys that need to execute to protect the system. But circuit breakers exist to prevent cascading liquidations.

**During COOLDOWN:**

Liquidation engine continues to detect undercollateralized positions.

Liquidation orders are placed as reduce-only IOC (same as normal).

BUT:

  \- Liquidation orders ARE subject to Layer 2 price impact bands

  \- If a liquidation order can't fill within the impact band → partial fill

  \- Unfilled portion → next liquidation iteration (same gradual liquidation model)

  \- New aggressive non-liquidation orders blocked → less selling pressure

  \- MMs can re-quote wider → provides fill targets for liquidations

Net effect: liquidations slow down (fewer aggressive sellers competing),

giving the book time to absorb them. This IS the mechanism that

prevents cascading liquidation spirals.

**During EMERGENCY:**

Liquidation engine continues. Liquidation is the one thing that

cannot stop — undercollateralized positions are a liability.

  \- Liquidation orders still fill against the book

  \- If book is empty: insurance vault absorbs (as per Flow 5 waterfall)

  \- If insurance vault depleted: ADL activates

The difference: no new position-opening orders means no new fuel

for the cascade. Only existing positions unwind. The fire burns

through existing fuel but gets no new oxygen.

### Admin controls

The admin dashboard (Component 29\) needs circuit breaker controls:

| Control | What it does | Who can use it |
| :---- | :---- | :---- |
| **Trigger EMERGENCY** | Force all instruments into EMERGENCY state | Admin (requires 2FA) |
| **Recover from EMERGENCY** | Return all instruments to TRADING | Admin (requires 2FA) |
| **Halt single instrument** | Force one instrument into EMERGENCY (independent of others) | Admin |
| **Adjust band parameters** | Change price band %, velocity threshold, cooldown duration | Admin (takes effect immediately) |
| **View breaker status** | Dashboard showing all instrument states, recent triggers, vault utilization | Admin \+ read-only ops |

**Audit requirement:** Every admin action on circuit breakers is logged with: who, when, what, and a mandatory reason field. These are safety-critical controls. The audit trail must be immutable.

### Monitoring and alerting

Circuit breaker events are high-priority operational events.

| Event | Alert level | Who gets notified |
| :---- | :---- | :---- |
| Layer 1 rejection spike (\>100/min) | Warning | Ops |
| Layer 2 impact band hit | Info | Logged, no alert |
| COOLDOWN triggered | High | Ops \+ engineering on-call |
| COOLDOWN → TRADING recovery | Info | Ops |
| EMERGENCY triggered (auto) | Critical | Ops \+ engineering \+ leadership |
| EMERGENCY triggered (manual) | Critical | Ops \+ engineering \+ leadership |
| Insurance vault utilization \> 70% | High | Ops \+ engineering |
| Insurance vault utilization \> 90% | Critical | Everyone |

### How TradFi compares

|  | TradFi (CME/NSE) | Asgard |
| :---- | :---- | :---- |
| **Price bands** | Dynamic bands around reference price (CME "price banding"). Identical concept. | Same — ±X% around mark price. |
| **Velocity controls** | CME "velocity logic" — detects rapid movement, triggers brief pause. | Same concept — velocity threshold triggers cooldown. |
| **Post-pause mechanism** | Volatility auction (CME, Eurex). Reopens with price discovery. | Cooldown timer (MVP). Volatility auction is V1.1. |
| **Market-wide breakers** | Yes. S\&P 500 based: 7%/13%/20% drops → 15-min halt / trading day halt. NSE: 10%/15%/20%. | Yes. Multi-instrument cooldown \+ vault stress → EMERGENCY. |
| **Scope** | Per-instrument \+ market-wide | Same. |
| **Withdrawals during halt** | N/A (different system) | Always open. Non-negotiable. |
| **Kill switch** | Yes — per participant, per market, per system. | Yes — per instrument and system-wide. Per-user kill switch in V1.1. |

The biggest gap: we don't have a volatility auction for MVP. TradFi uses auctions to establish a fair price after a halt. We use a timer, which assumes the market will self-correct when the cooldown ends. This works if MMs are active and re-quote during cooldown. It fails if MMs withdraw entirely. The auction mechanism is important for V1.1 — it gives the market a structured way to discover the new price rather than hoping MMs step up.

### System Diagram — Circuit Breakers

┌───────────────────────────────────────────────────────────────────────┐

│                                                                       │

│  ┌─────────────┐     ┌──────────────────┐     ┌──────────────────┐   │

│  │ Order        │────►│ Pre-Trade Risk   │────►│ Matching Engine  │   │

│  │ (from user)  │     │                  │     │                  │   │

│  └─────────────┘     │ Layer 1:         │     │ Layer 2:         │   │

│                       │ Limit Price Band │     │ Price Impact Band│   │

│                       │ → REJECT         │     │ → STOP MATCHING  │   │

│                       └──────────────────┘     └────────┬─────────┘   │

│                                                          │            │

│                                              trade executes           │

│                                                          │            │

│                                                          ▼            │

│                                              ┌──────────────────┐    │

│  ┌─────────────┐                             │ Mark Price Engine │    │

│  │ Instrument   │◄───── state change ────────│                  │    │

│  │ Manager      │                             │ Layer 3:         │    │

│  │              │                             │ Velocity Detect  │    │

│  │ Manages:     │                             │ → COOLDOWN       │    │

│  │ TRADING      │                             └──────────────────┘    │

│  │ COOLDOWN     │                                                     │

│  │ EMERGENCY    │◄───── admin trigger ──── ┌──────────────────┐      │

│  │              │                           │ Admin Dashboard   │      │

│  │              │◄───── vault stress ───── │                  │      │

│  │              │                           └──────────────────┘      │

│  └──────┬───────┘                                                     │

│         │                                                             │

│         │ state broadcast                                             │

│         ▼                                                             │

│  ┌──────────────────┐                                                 │

│  │ All components    │                                                │

│  │ check instrument  │                                                │

│  │ state before      │                                                │

│  │ processing        │                                                │

│  └──────────────────┘                                                 │

│                                                                       │

│  ┌──────────────────┐                                                 │

│  │ Insurance Vault   │── utilization ──► Layer 4: Market-Wide        │

│  │ (Component 12\)    │                   Breaker                      │

│  └──────────────────┘                   (triggers EMERGENCY           │

│                                          if utilization \> 90%)        │

└───────────────────────────────────────────────────────────────────────┘

### Decisions Made — Flow 8

| \# | Decision | Choice | Rationale |
| :---- | :---- | :---- | :---- |
| 1 | Protection model | 4 layers: limit band, impact band, velocity, market-wide breaker | Each layer catches what the previous missed. Industry standard (CME has same layering). |
| 2 | Layer 1 reference price | Mark price | Manipulation-resistant (anchored to oracle index via EWMA). |
| 3 | Layer 2 reference price | Best bid/ask at order arrival | Measures the order's own market impact, not deviation from an external reference. |
| 4 | Cooldown mode (not full halt) | Restrict new aggression, allow liquidity provision and closes | 24/7 market has no auction mechanism to reopen after halt (MVP). Cooldown lets market heal. |
| 5 | Withdrawals during EMERGENCY | Always open | Non-negotiable trust principle. Blocking withdrawals \= FTX. |
| 6 | EMERGENCY recovery | Manual admin only | Automated recovery risks returning to trading while underlying cause persists. Human judgment required. |
| 7 | Liquidation during circuit breaker | Continues, subject to impact bands | Liquidation cannot stop — undercollateralized positions are a systemic liability. Impact bands limit cascade speed. |
| 8 | Expiry vs circuit breaker priority | Expiry wins — settlement cannot be delayed by breakers | Contract expiry is a hard deadline. Users and counterparties depend on it. |
| 9 | Volatility auction | V1.1 — not MVP | Requires building an auction engine. Timer-based cooldown is sufficient for launch with active MMs. |
| 10 | Per-user kill switch | V1.1 | Cancel all orders for a specific user. Useful for rogue algo detection. MVP has instrument-level and system-level. |

### Open Questions — Flow 8

- [ ] Price band percentages per instrument — 5% for BTC, 3% for XAU? Need to validate against historical volatility data.  
- [ ] Impact band percentage — 2%? Should it be tighter for thinner books?  
- [ ] Velocity threshold and window — 8% in 60s for BTC? Need to backtest against historical BTC moves to set thresholds that trigger on genuine crashes but not on normal volatility.  
- [ ] Cooldown duration — 30 seconds? 60 seconds? Too short and it's useless. Too long and it blocks legitimate trading.  
- [ ] Insurance vault emergency threshold — 90% utilization? Or lower (80%) to give more buffer?  
- [ ] Multi-instrument cooldown threshold — 3+ instruments? With only 2 instruments at launch (BTC, XAU), this effectively means "both instruments in cooldown \= emergency." Is that the right trigger?  
- [ ] Should cooldown tighten price bands? E.g., during cooldown, limit bands narrow from ±5% to ±2% to further restrict aggressive pricing.  
- [ ] MM obligations during cooldown — should designated MMs be required to maintain quotes during cooldown? This is how TradFi works (CME obligations persist during volatility events). Needs to be in the MM agreement.  
- [ ] Volatility auction design for V1.1 — call auction or batch auction? What's the minimum viable auction mechanism?  
- [ ] Should Layer 2 (impact band) apply differently for liquidation orders vs. user orders? Liquidations arguably need wider bands since they're forced.

---


Everything in one place. This section ties all 10 flows together into a single picture of the complete exchange. Every component from `02-component-map.md`, every connection documented in Flows 1–10, every data path — unified.

### Component inventory

32 components across 5 categories. Here they are grouped by function, with the flow(s) each component participates in.

**Order pipeline (the critical path — latency-sensitive):**

| \# | Component | Flows | Role in the system |
| :---- | :---- | :---- | :---- |
| 1 | API Gateway | 1, 2, 7 | Entry point. Authenticates (ED25519), parses, routes. REST \+ WebSocket \+ FIX (V1.1). |
| 2 | Sequencer | 2, 3 | Assigns deterministic sequence numbers. Single-writer WAL. Crash recovery via replay. |
| 3 | Pre-Trade Risk Engine | 2, 8 | Validates every order: instrument state, price bands (Layer 1), size, rate limit, position limit, margin, self-trade prevention. |
| 4 | Matching Engine | 2, 3, 5, 8 | CLOB. Price-time FIFO. Produces trade events. Enforces price impact bands (Layer 2). |
| 5 | Order Management System | 2, 3, 5 | Tracks order lifecycle (new → open → partial → filled / cancelled). Manages stop trigger queue. |

**Risk and margin (the safety layer — correctness-critical):**

| \# | Component | Flows | Role in the system |
| :---- | :---- | :---- | :---- |
| 6 | Risk Engine | 3, 4, 5, 7 | Continuous margin check every mark price tick. Detects undercollateralized positions. Sends margin warnings. |
| 7 | Liquidation Engine | 5, 8, 9 | Gradual liquidation (\~10% per iteration). Cancel orders → re-check → sell chunks → insurance vault → ADL. |
| 8 | Mark Price Engine | 4, 5, 7, 8, 9 | Index \+ 1-min EWMA of (mid \- index), bounded ±5%. Feeds risk engine, liquidation, stops, market data. Oracle failure detection. |
| 12 | Insurance Vault | 5, 6, 8 | Open vault. Exchange SITG \+ external depositors. Absorbs bankrupt liquidation deficits. Utilization gate for withdrawals. |
| 13 | ADL Engine | 5 | Last resort. Auto-deleverages profitable traders when vault depleted. Ranked by PnL% × leverage. |

**Settlement and instrument management:**

| \# | Component | Flows | Role in the system |
| :---- | :---- | :---- | :---- |
| 9 | Settlement Price Calculator | 6, 9 | 30-min TWAP of oracle index at expiry. Fallback to secondary oracle. Extend window on failure. |
| 11 | Instrument Manager | 6, 8, 9, 10 | Contract lifecycle: template → PRE\_OPEN → TRADING → CLOSE\_ONLY → SETTLING → SETTLED → DELISTED. Auto-lists replacements. Manages COOLDOWN / ORACLE\_HALT / EMERGENCY states. |
| 14 | Internal Ledger | 1, 3, 4, 5, 6, 7 | Source of truth. All balances, positions, margin, PnL. The core accounting system. Zero-sum invariant. |

**Market data and distribution:**

| \# | Component | Flows | Role in the system |
| :---- | :---- | :---- | :---- |
| 10 | Market Data Engine | 7 | Aggregates: depth deltas, trades, ticker, klines, BBO, OI, mark price, liquidation events. Fans out via pub/sub to WebSocket pods. |

**Custody and fund management:**

| \# | Component | Flows | Role in the system |
| :---- | :---- | :---- | :---- |
| 15 | MPC Infrastructure | 1 | Multi-party signing. No single key authorizes withdrawals. Fireblocks or equivalent. |
| 16 | Deposit Wallets | 1 | Per-user addresses across Solana, Ethereum, Arbitrum. |
| 17 | Hot Wallet | 1 | \~2–5% of funds. Processes withdrawals. |
| 18 | Warm Storage | 1 | \~20–30% of funds. Tops up hot wallet. |
| 19 | Cold Storage | 1 | \~65–75% of funds. Multisig, bank vault keys. |
| 20 | Chain Listener | 1 | Monitors deposit wallets. Detects incoming USDC, credits ledger. |
| 21 | Fund Sweeper | 1 | Moves funds between tiers based on thresholds. |

**Oracle layer:**

| \# | Component | Flows | Role in the system |
| :---- | :---- | :---- | :---- |
| 22 | Oracle Integration | 4, 6, 8, 9 | Reads Pyth (primary) \+ Switchboard (secondary). Staleness detection. Deviation check. Confidence monitoring. Failover logic. |

**Client-facing:**

| \# | Component | Flows | Role in the system |
| :---- | :---- | :---- | :---- |
| 23 | Trading UI | 7 | Web/mobile frontend. Consumes market data, displays orderbook, chart, positions, PnL. |
| 24 | API (REST \+ WebSocket) | 1, 2, 7 | Programmatic access. REST for snapshots/queries. WebSocket for streaming \+ order entry. ED25519 auth. |
| 25 | Wallet Integration | 1 | Crypto wallet connection for deposits (Phantom, MetaMask). |

**Operations and infrastructure:**

| \# | Component | Flows | Role in the system |
| :---- | :---- | :---- | :---- |
| 26 | Audit Trail / Event Log | 1, 2, 3, 5, 6, 8 | Immutable log. Every order, trade, liquidation, settlement, deposit, withdrawal. Sequential, timestamped. |
| 27 | Surveillance / Monitoring | 8 | Wash trade detection, spoofing detection, unusual activity alerts. |
| 28 | Circuit Breakers | 8 | 4 layers: limit band, impact band, velocity detection, market-wide breaker. Manages COOLDOWN / EMERGENCY states. |
| 29 | Admin Dashboard | 8, 9, 10 | Kill switches, circuit breaker controls, instrument management, oracle monitoring, vault status. |
| 30 | KYC / Onboarding | 1 | User identity verification. Scope depends on regulatory approach. |
| 31 | Reconciliation Engine | 1 | Every 5 min: on-chain wallet balances vs internal ledger totals. Alert on divergence. |
| 32 | Proof of Reserves | 1 | Daily Merkle tree. Root hash on-chain. User-verifiable. |

### The complete system diagram

┌─────────────────────────────────────────────────────────────────────────────────────┐

│                                    CLIENTS                                           │

│                                                                                      │

│  ┌────────────────────┐   ┌────────────────────┐   ┌──────────────────────┐          │

│  │  Trading UI \[23\]   │   │  API Clients \[24\]   │   │  Wallet App \[25\]    │          │

│  │  (Web / Mobile)    │   │  (MMs, bots, algos) │   │  (Phantom, MetaMask)│          │

│  └─────────┬──────────┘   └─────────┬──────────┘   └──────────┬─────────┘          │

│            │ WebSocket \+ REST        │ WebSocket \+ REST         │ on-chain tx        │

└────────────┼─────────────────────────┼──────────────────────────┼────────────────────┘

             │                         │                          │

             └────────────┬────────────┘                          │

                          │                                       │

                          ▼                                       ▼

┌─────────────────────────────────────────────────────────────────────────────────────┐

│                            API GATEWAY \[1\]                                            │

│                                                                                      │

│  • ED25519 authentication          • WebSocket connection management                 │

│  • REST request routing             • Rate limiting (per user tier)                   │

│  • Format validation                • WebSocket auth for private streams              │

└──────────────────────────────┬──────────────────────────────────────────────────────┘

                               │

              ┌────────────────┼────────────────┐

              │ orders/cancels │                 │ queries

              ▼                │                 ▼

┌──────────────────────┐      │    ┌────────────────────────────────────┐

│  SEQUENCER \[2\]       │      │    │  REST API LAYER                    │

│                      │      │    │                                    │

│  • Assign seq number │      │    │  /depth  /trades  /ticker  /klines │

│  • Write to WAL      │      │    │  /instruments  /markPrices         │

│  • Crash recovery    │      │    │  /positions  /balances  /orders    │

│  • Single-writer     │      │    │                                    │

└──────────┬───────────┘      │    │  (reads from Market Data Engine    │

           │                  │    │   snapshots \+ Internal Ledger)     │

           ▼                  │    └────────────────────────────────────┘

┌──────────────────────────┐  │

│  PRE-TRADE RISK \[3\]      │  │

│                          │  │

│  □ Instrument state      │◄─┼──── Instrument Manager \[11\] (state check)

│    (TRADING? COOLDOWN?)  │  │

│  □ Price band (Layer 1\)  │◄─┼──── Mark Price Engine \[8\] (reference price)

│  □ Size limits           │  │

│  □ Rate limit            │  │

│  □ Position limit        │  │

│  □ Margin check          │◄─┼──── Internal Ledger \[14\] (available balance)

│  □ Self-trade prevention │  │

│                          │  │

│  REJECT ──► WebSocket    │  │

│             error to user│  │

└──────────┬───────────────┘  │

           │ pass             │

           ▼                  │

┌──────────────────────────┐  │

│  MATCHING ENGINE \[4\]     │  │

│                          │  │

│  • CLOB, price-time FIFO │  │

│  • Price impact band     │  │

│    (Layer 2\) per fill    │  │

│  • Produces:             │  │

│    \- TradeEvent          │  │

│    \- Book changes        │  │

│    \- Order state changes │  │

└──────────┬───────────────┘  │

           │                  │

           │ trade events \+ book changes \+ order updates

           │

           ▼

┌─────────────────────────────────────────────────────────────────────────────────────┐

│                           INTERNAL EVENT BUS                                         │

│                                                                                      │

│  Every event from the matching engine is published here.                              │

│  Consumers process independently. The matching engine never blocks on consumers.      │

│                                                                                      │

│  Events: TradeEvent, BookUpdate, OrderUpdate, LiquidationEvent,                      │

│          SettlementEvent, MarkPriceUpdate, InstrumentStateChange                     │

└──┬──────────┬──────────┬──────────┬───────────┬──────────┬──────────┬───────────────┘

   │          │          │          │           │          │          │

   ▼          ▼          ▼          ▼           ▼          ▼          ▼

┌─────────────────────────────────────────────────────────────────────────────────────┐

│                        EVENT BUS CONSUMERS                                           │

│                                                                                      │

│  ┌────────────────────────┐   ┌──────────────────────────────────────────────┐       │

│  │ INTERNAL LEDGER \[14\]   │   │ RISK ENGINE \[6\]                              │       │

│  │                        │   │                                              │       │

│  │ On TradeEvent:         │   │ On MarkPriceUpdate (every tick):             │       │

│  │  • Fee deduction       │   │  • Recalculate unrealized\_pnl per position   │       │

│  │  • Position create/    │   │  • Recalculate equity per user               │       │

│  │    update/close        │   │  • Compare equity vs MM                      │       │

│  │  • PnL realization     │   │  • SAFE / WARNING / LIQUIDATE                │       │

│  │  • Margin recalc       │   │                                              │       │

│  │  • OI tracking         │   │ On TradeEvent:                               │       │

│  │                        │   │  • Post-trade risk check for both parties    │       │

│  │ On SettlementEvent:    │   │                                              │       │

│  │  • Force-close PnL     │   │  WARNING ──► WebSocket to user               │       │

│  │  • Free all margin     │   │  LIQUIDATE ──► Liquidation Engine \[7\]        │       │

│  │                        │   │                                              │       │

│  │ Zero-sum invariant:    │   └──────────────────────────────────────────────┘       │

│  │ Σ realized \+ Σ unreal  │                                                          │

│  │ \= 0 (always)           │   ┌──────────────────────────────────────────────┐       │

│  └────────────────────────┘   │ ORDER MANAGEMENT SYSTEM \[5\]                  │       │

│                                │                                              │       │

│                                │ • Tracks order lifecycle                     │       │

│                                │   (new → open → partial → filled/cancelled) │       │

│                                │ • Manages stop trigger queue                │       │

│                                │ • Scans stops on every MarkPriceUpdate      │       │

│                                │ • Triggered stops → Sequencer → pipeline    │       │

│                                └──────────────────────────────────────────────┘       │

│                                                                                      │

│  ┌────────────────────────┐   ┌──────────────────────────────────────────────┐       │

│  │ MARKET DATA ENGINE \[10\]│   │ AUDIT TRAIL \[26\]                             │       │

│  │                        │   │                                              │       │

│  │ Consumes:              │   │ Immutable log of every event.                │       │

│  │  • TradeEvent          │   │ Sequential. Timestamped. Never deleted.      │       │

│  │  • BookUpdate          │   │                                              │       │

│  │  • MarkPriceUpdate     │   │ Orders, trades, liquidations, settlements,   │       │

│  │  • LiquidationEvent    │   │ deposits, withdrawals, state changes,        │       │

│  │  • OI changes          │   │ admin actions, circuit breaker events.       │       │

│  │                        │   │                                              │       │

│  │ Produces:              │   └──────────────────────────────────────────────┘       │

│  │  • depth deltas \+ seq  │                                                          │

│  │  • trade stream        │   ┌──────────────────────────────────────────────┐       │

│  │  • ticker (aggregated) │   │ SURVEILLANCE / MONITORING \[27\]               │       │

│  │  • BBO                 │   │                                              │       │

│  │  • kline construction  │   │ • Wash trade detection                       │       │

│  │  • OI stream           │   │ • Spoofing / layering detection              │       │

│  │  • liquidation stream  │   │ • Unusual activity alerts                    │       │

│  │  • mark price stream   │   │ • Front-running detection                    │       │

│  │  • REST snapshots      │   │                                              │       │

│  └───────────┬────────────┘   └──────────────────────────────────────────────┘       │

│              │                                                                       │

└──────────────┼───────────────────────────────────────────────────────────────────────┘

               │

               │  pub/sub topics (depth.X, trade.X, ticker.X, account.orders.USER, ...)

               ▼

┌─────────────────────────────────────────────────────────────────────────────────────┐

│                        WEBSOCKET FANOUT LAYER                                        │

│                                                                                      │

│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐                            │

│  │  WS Pod  │  │  WS Pod  │  │  WS Pod  │  │  WS Pod  │  ... N pods               │

│  │    1     │  │    2     │  │    3     │  │    4     │  (horizontally scalable)   │

│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └────┬─────┘                            │

│       │             │             │             │                                    │

│       ▼             ▼             ▼             ▼                                    │

│    clients       clients       clients       clients                                 │

│                                                                                      │

│  Each pod subscribes to topics its clients need.                                     │

│  One event copy per pod, fanned out to all subscribers on that pod.                  │

│  Bounded send buffers. Slow consumers disconnected.                                  │

│  Public streams: no auth. Private streams: ED25519 authenticated.                    │

└─────────────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────────────┐

│                     LIQUIDATION & LOSS ABSORPTION                                    │

│                                                                                      │

│  Risk Engine detects: equity \<= MM                                                   │

│       │                                                                              │

│       ▼                                                                              │

│  ┌──────────────────────────────┐                                                    │

│  │  LIQUIDATION ENGINE \[7\]      │                                                    │

│  │                              │                                                    │

│  │  1\. Cancel all open orders   │                                                    │

│  │  2\. Re-check margin          │──► restored? STOP                                  │

│  │  3\. Select position          │                                                    │

│  │     (largest loss first)     │                                                    │

│  │  4\. Gradual loop:            │                                                    │

│  │     • 10% chunk              │                                                    │

│  │     • Reduce-only IOC        │──► Matching Engine \[4\] (fills on orderbook)        │

│  │     • 1% fee → vault         │                                                    │

│  │     • Re-check after each    │                                                    │

│  │  5\. If fully closed:         │                                                    │

│  │     equity \>= 0 → done       │                                                    │

│  │     equity \<  0 → BANKRUPT   │                                                    │

│  └──────────────┬───────────────┘                                                    │

│                 │ bankrupt deficit                                                    │

│                 ▼                                                                     │

│  ┌──────────────────────────────┐                                                    │

│  │  INSURANCE VAULT \[12\]        │                                                    │

│  │                              │                                                    │

│  │  • Exchange SITG (locked)    │                                                    │

│  │  • External depositors (APY) │                                                    │

│  │  • Absorbs bankrupt deficits │                                                    │

│  │  • Funded by 1% liq fees     │                                                    │

│  │  • Utilization gate:         │                                                    │

│  │    \<50%: 7d cooldown         │                                                    │

│  │    50-80%: 14d, partial only │                                                    │

│  │    \>80%: withdrawals blocked │                                                    │

│  │    \>90%: EMERGENCY trigger   │──► Circuit Breakers \[28\]                           │

│  │                              │                                                    │

│  │  Vault depleted?             │                                                    │

│  └──────────────┬───────────────┘                                                    │

│                 │ vault cannot cover deficit                                          │

│                 ▼                                                                     │

│  ┌──────────────────────────────┐                                                    │

│  │  ADL ENGINE \[13\]             │                                                    │

│  │                              │                                                    │

│  │  Last resort.                │                                                    │

│  │  Force-close profitable      │                                                    │

│  │  traders against bankrupt.   │                                                    │

│  │  Ranked: PnL% × leverage.   │                                                    │

│  │  Highest priority first.     │                                                    │

│  └──────────────────────────────┘                                                    │

└─────────────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────────────┐

│                     ORACLE & MARK PRICE                                              │

│                                                                                      │

│      ┌───────────────┐      ┌───────────────┐                                       │

│      │  Pyth Network │      │  Switchboard  │                                       │

│      │  (primary)    │      │  (secondary)  │                                       │

│      │  \~400ms       │      │  \~1-5s        │                                       │

│      └───────┬───────┘      └───────┬───────┘                                       │

│              │                       │                                                │

│              ▼                       ▼                                                │

│      ┌──────────────────────────────────────────┐                                    │

│      │  ORACLE INTEGRATION \[22\]                  │                                   │

│      │                                           │                                   │

│      │  • Staleness detection (5s warn, 30s crit)│                                   │

│      │  • Deviation check (\>20% \= reject)        │                                   │

│      │  • Confidence interval monitoring (Pyth)  │                                   │

│      │  • Cross-oracle comparison                │                                   │

│      │  • Failover: Pyth → Switchboard           │                                   │

│      │  • State: NORMAL / DEGRADED / HALT        │                                   │

│      └─────────────────┬─────────────────────────┘                                   │

│                        │ validated price \+ oracle state                               │

│                        ▼                                                              │

│      ┌──────────────────────────────────────────┐                                    │

│      │  MARK PRICE ENGINE \[8\]                    │                                   │

│      │                                           │                                   │

│      │  Formula:                                 │                                   │

│      │    mark \= index \+ EWMA(mid \- index, 1min) │                                   │

│      │    bounded: \[index×0.95, index×1.05\]      │                                   │

│      │                                           │                                   │

│      │  Fallback hierarchy:                      │                                   │

│      │    1\. Index \+ EWMA      (normal)          │                                   │

│      │    2\. Index alone       (no book quotes)  │                                   │

│      │    3\. Median {bid,ask,last} (oracle stale)│                                   │

│      │    4\. Mid price         (degraded)        │                                   │

│      │    5\. Last traded       (emergency)       │                                   │

│      │                                           │                                   │

│      │  On oracle halt: FREEZE mark price        │                                   │

│      └──────┬──────────────────┬─────────────────┘                                   │

│             │                  │                                                      │

│             │                  │  mark\_price feeds into:                              │

│             ▼                  ▼                                                      │

│     ┌──────────────┐  ┌──────────────────┐  ┌──────────────────────┐                │

│     │ Risk Engine  │  │ Pre-Trade Risk   │  │ OMS (stop trigger    │                │

│     │ \[6\]          │  │ \[3\]              │  │ queue) \[5\]           │                │

│     │ margin calc, │  │ price bands,     │  │ triggers stops on    │                │

│     │ liquidation  │  │ Layer 1 check    │  │ mark/last/index      │                │

│     │ triggers     │  │                  │  │                      │                │

│     └──────────────┘  └──────────────────┘  └──────────────────────┘                │

│             │                                                                        │

│             │  also feeds:                                                            │

│             ▼                                                                        │

│     ┌──────────────────────┐  ┌──────────────────────────┐                           │

│     │ Market Data Engine   │  │ Settlement Price Calc \[9\] │                          │

│     │ \[10\]                 │  │                           │                          │

│     │ mark price stream    │  │ 30-min TWAP at expiry     │                          │

│     │ to all subscribers   │  │ (via Oracle Integration)  │                          │

│     └──────────────────────┘  └──────────────────────────┘                           │

└─────────────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────────────┐

│                     CIRCUIT BREAKERS & PROTECTION \[28\]                                │

│                                                                                      │

│  ┌────────────────────────────────────────────────────────────────────────────┐      │

│  │  LAYER 1: Limit Price Band               (Pre-Trade Risk \[3\])             │      │

│  │  Order price within ±X% of mark? If not → REJECT order.                   │      │

│  ├────────────────────────────────────────────────────────────────────────────┤      │

│  │  LAYER 2: Price Impact Band              (Matching Engine \[4\])            │      │

│  │  Fill would move price \>Y% from best? If so → STOP matching.             │      │

│  ├────────────────────────────────────────────────────────────────────────────┤      │

│  │  LAYER 3: Velocity Detection             (Mark Price Engine \[8\])          │      │

│  │  Mark moved \>Z% in 60s? → COOLDOWN: block new aggression, allow          │      │

│  │  liquidity provision \+ reduces. Timer-based recovery.                     │      │

│  ├────────────────────────────────────────────────────────────────────────────┤      │

│  │  LAYER 4: Market-Wide Breaker            (Instrument Manager \[11\])        │      │

│  │  3+ instruments in COOLDOWN, OR vault utilization \>90%, OR admin          │      │

│  │  kill switch → EMERGENCY: reduce-only all instruments. Manual recovery.   │      │

│  └────────────────────────────────────────────────────────────────────────────┘      │

│                                                                                      │

│  Instrument state transitions:                                                       │

│                                                                                      │

│  TRADING ──► COOLDOWN ──► TRADING          (automatic timer)                         │

│  TRADING ──► ORACLE\_HALT ──► TRADING       (oracle recovery \+ admin confirm)         │

│  Any ──────► EMERGENCY ──► TRADING         (manual admin recovery only)              │

│                                                                                      │

│  Priority: expiry always wins. COOLDOWN/EMERGENCY → CLOSE\_ONLY at T-10min.          │

│  Withdrawals ALWAYS open. Non-negotiable.                                            │

└─────────────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────────────┐

│                     CONTRACT LIFECYCLE \[11\]                                           │

│                                                                                      │

│  ┌──────────────────────────────────────────────────────────┐                        │

│  │  INSTRUMENT TEMPLATES (admin-managed)                     │                       │

│  │                                                           │                       │

│  │  BTC-USDC-WEEKLY:  margin, tick/lot, fees, oracle, N=4   │                       │

│  │  BTC-USDC-MONTHLY: margin, tick/lot, fees, oracle, N=2   │                       │

│  │  XAU-USDC-WEEKLY:  margin, tick/lot, fees, oracle, N=4   │                       │

│  │  XAU-USDC-MONTHLY: margin, tick/lot, fees, oracle, N=2   │                       │

│  └──────────────────────────┬───────────────────────────────┘                        │

│                             │                                                        │

│                             ▼                                                        │

│  ┌──────────────────────────────────────────────────────────┐                        │

│  │  INSTRUMENT MANAGER \[11\]                                  │                       │

│  │                                                           │                       │

│  │  Auto-listing pipeline:                                   │                       │

│  │    T-24h: validate oracle → create PRE\_OPEN              │                       │

│  │    T:     settlement of expiring contract                │                       │

│  │    T+30m: new contract → TRADING                         │                       │

│  │    T+24h: old contract → DELISTED                        │                       │

│  │                                                           │                       │

│  │  Continuous:                                              │                       │

│  │    • Maintain N active contracts per cadence              │                       │

│  │    • Enforce expiry schedule (CLOSE\_ONLY at T-10m)       │                       │

│  │    • Manage COOLDOWN/ORACLE\_HALT/EMERGENCY states        │                       │

│  │    • Publish state changes → Event Bus → all consumers   │                       │

│  └──────────────────────────────────────────────────────────┘                        │

│                                                                                      │

│  Contract lifecycle:                                                                 │

│                                                                                      │

│    PRE\_OPEN ──► TRADING ──► CLOSE\_ONLY ──► SETTLING ──► SETTLED ──► DELISTED        │

│    (T-24h)      (live)      (T-10min)      (T to T+30m)  (done)     (T+24h)         │

│                    │                                                                 │

│                    ├──► COOLDOWN ──► TRADING     (velocity, auto-timer)               │

│                    ├──► ORACLE\_HALT ──► TRADING  (oracle fail, admin confirm)         │

│                    └──► EMERGENCY ──► TRADING    (systemic, admin only)               │

└─────────────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────────────┐

│                     CUSTODY & FUND MANAGEMENT                                        │

│                                                                                      │

│  ┌─────────────────────────────────────────────────────────────────────────────┐     │

│  │                          BLOCKCHAINS                                        │     │

│  │                                                                             │     │

│  │   Solana              Ethereum            Arbitrum          ...more         │     │

│  │   ┌──────────┐        ┌──────────┐        ┌──────────┐                     │     │

│  │   │ Deposit  │        │ Deposit  │        │ Deposit  │                     │     │

│  │   │ Wallets  │        │ Wallets  │        │ Wallets  │                     │     │

│  │   │ \[16\]     │        │ \[16\]     │        │ \[16\]     │                     │     │

│  │   └────┬─────┘        └────┬─────┘        └────┬─────┘                     │     │

│  └────────┼────────────────────┼──────────────────┼───────────────────────────┘     │

│           │                    │                   │                                  │

│           └────────────┬───────┘───────────────────┘                                 │

│                        │ incoming USDC                                                │

│                        ▼                                                              │

│           ┌──────────────────────┐                                                   │

│           │  CHAIN LISTENER \[20\] │ monitors all deposit wallets, all chains           │

│           │  deduplicates by     │ credits Internal Ledger \[14\]                       │

│           │  tx\_hash             │                                                    │

│           └──────────────────────┘                                                   │

│                                                                                      │

│           ┌──────────────────────┐                                                   │

│           │  FUND SWEEPER \[21\]   │ deposit wallets → hot → warm → cold               │

│           └──────────────────────┘ threshold-based, periodic                          │

│                                                                                      │

│           Fund tiers:                                                                │

│           ┌─────────────┐  ┌──────────────┐  ┌──────────────┐                       │

│           │ Hot Wallet   │  │ Warm Storage  │  │ Cold Storage  │                      │

│           │ \[17\]         │  │ \[18\]          │  │ \[19\]          │                      │

│           │ \~2-5%        │  │ \~20-30%       │  │ \~65-75%       │                      │

│           │ withdrawals  │  │ tops up hot   │  │ multisig,     │                      │

│           └──────┬───────┘  └───────────────┘  │ bank vaults   │                      │

│                  │                              └───────────────┘                      │

│                  │ withdrawal signing                                                 │

│                  ▼                                                                    │

│           ┌──────────────────────────────────┐                                       │

│           │  MPC INFRASTRUCTURE \[15\]          │                                      │

│           │  (Fireblocks or equivalent)       │                                      │

│           │                                   │                                      │

│           │  Key Share A \+ B \+ C              │                                      │

│           │  Multiple shares sign withdrawal  │                                      │

│           │  No single key can authorize      │                                      │

│           └──────────────┬───────────────────┘                                       │

│                          │ signed tx → blockchain                                     │

│                          ▼                                                            │

│           USDC arrives in user's external wallet                                     │

│                                                                                      │

│  ┌────────────────────┐  ┌────────────────────┐                                     │

│  │ RECONCILIATION \[31\]│  │ PROOF OF            │                                    │

│  │                    │  │ RESERVES \[32\]        │                                    │

│  │ Every 5 min:       │  │                     │                                    │

│  │ Σ on-chain wallets │  │ Daily Merkle tree   │                                    │

│  │ \= Σ internal ledger│  │ Root hash on-chain  │                                    │

│  │ Alert on mismatch  │  │ User-verifiable     │                                    │

│  └────────────────────┘  └─────────────────────┘                                    │

└─────────────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────────────┐

│                     OPERATIONS                                                       │

│                                                                                      │

│  ┌──────────────────────────────────────────────────────────────────────────────┐    │

│  │  ADMIN DASHBOARD \[29\]                                                        │    │

│  │                                                                              │    │

│  │  • Circuit breaker controls (trigger/recover EMERGENCY, adjust bands)        │    │

│  │  • Instrument management (create template, suspend, force-delist)            │    │

│  │  • Oracle monitoring (feed health, staleness, cross-oracle divergence)       │    │

│  │  • Insurance vault status (utilization, deposits, withdrawals)               │    │

│  │  • System health (matching engine latency, WS connections, event bus lag)    │    │

│  │  • Fund management (hot/warm/cold balances, sweep triggers)                 │    │

│  │  • Kill switches (per instrument, system-wide)                              │    │

│  │  • All admin actions require 2FA, logged to Audit Trail \[26\]                │    │

│  └──────────────────────────────────────────────────────────────────────────────┘    │

│                                                                                      │

│  ┌──────────────────────────────────────────────────────────────────────────────┐    │

│  │  KYC / ONBOARDING \[30\]                                                       │    │

│  │                                                                              │    │

│  │  User identity verification. Scope depends on regulatory approach.           │    │

│  │  Required before deposits/trading enabled.                                   │    │

│  └──────────────────────────────────────────────────────────────────────────────┘    │

└─────────────────────────────────────────────────────────────────────────────────────┘

### Data flow summary — the complete picture in one table

Every arrow in the system. Source component → destination component, what data flows, which flow documents it.

**Order path (hot path, latency-critical):**

| From | To | Data | Flow |
| :---- | :---- | :---- | :---- |
| Client | API Gateway \[1\] | Order / cancel / modify request | 2 |
| API Gateway \[1\] | Sequencer \[2\] | Validated, authenticated request | 2 |
| Sequencer \[2\] | Pre-Trade Risk \[3\] | Sequenced order (with seq\#) | 2 |
| Pre-Trade Risk \[3\] | Matching Engine \[4\] | Risk-approved order | 2 |
| Matching Engine \[4\] | Event Bus | TradeEvent, BookUpdate, OrderUpdate | 2, 3 |

**Post-trade processing:**

| From | To | Data | Flow |
| :---- | :---- | :---- | :---- |
| Event Bus | Internal Ledger \[14\] | TradeEvent → position update, fee, PnL | 3 |
| Event Bus | Risk Engine \[6\] | TradeEvent → post-trade risk check | 3, 4 |
| Event Bus | OMS \[5\] | OrderUpdate → state change, stop queue management | 2, 3 |
| Event Bus | Market Data Engine \[10\] | All events → aggregation, distribution | 7 |
| Event Bus | Audit Trail \[26\] | All events → immutable log | 1–10 |
| Event Bus | Surveillance \[27\] | All events → pattern analysis | 8 |

**Risk and liquidation:**

| From | To | Data | Flow |
| :---- | :---- | :---- | :---- |
| Mark Price Engine \[8\] | Risk Engine \[6\] | mark\_price updates (every tick) | 4 |
| Risk Engine \[6\] | Liquidation Engine \[7\] | Liquidation trigger (equity ≤ MM) | 5 |
| Liquidation Engine \[7\] | Matching Engine \[4\] | Reduce-only IOC orders (liquidation) | 5 |
| Liquidation Engine \[7\] | Insurance Vault \[12\] | 1% liquidation fee; bankrupt deficit absorption | 5 |
| Insurance Vault \[12\] | ADL Engine \[13\] | Vault depleted signal | 5 |
| ADL Engine \[13\] | Internal Ledger \[14\] | Force-close profitable positions | 5 |

**Oracle and mark price:**

| From | To | Data | Flow |
| :---- | :---- | :---- | :---- |
| Pyth / Switchboard | Oracle Integration \[22\] | Raw price feeds | 9 |
| Oracle Integration \[22\] | Mark Price Engine \[8\] | Validated index price \+ oracle state | 9 |
| Mark Price Engine \[8\] | Pre-Trade Risk \[3\] | mark\_price (for price bands) | 2, 8 |
| Mark Price Engine \[8\] | OMS \[5\] | mark\_price (for stop triggers) | 2 |
| Mark Price Engine \[8\] | Market Data Engine \[10\] | mark\_price stream | 7 |
| Mark Price Engine \[8\] | Circuit Breakers \[28\] | Price velocity data | 8 |
| Oracle Integration \[22\] | Settlement Price Calc \[9\] | Oracle index (for TWAP at expiry) | 6 |

**Market data distribution:**

| From | To | Data | Flow |
| :---- | :---- | :---- | :---- |
| Market Data Engine \[10\] | WebSocket Fanout Layer | Public: depth, trades, ticker, klines, BBO, OI, mark, liquidations | 7 |
| Risk Engine \[6\] | WebSocket Fanout Layer | Private: position updates, balance updates, margin warnings | 7 |
| Internal Ledger \[14\] | WebSocket Fanout Layer | Private: order updates (via OMS \[5\]) | 7 |
| WebSocket Fanout Layer | Clients | All streams, topic-based routing | 7 |

**Contract lifecycle:**

| From | To | Data | Flow |
| :---- | :---- | :---- | :---- |
| Instrument Manager \[11\] | All components | Instrument state changes (PRE\_OPEN, TRADING, CLOSE\_ONLY, etc.) | 10 |
| Instrument Manager \[11\] | Settlement Price Calc \[9\] | Trigger: begin TWAP sampling | 6 |
| Settlement Price Calc \[9\] | Internal Ledger \[14\] | Settlement price → force-close all positions | 6 |
| Instrument Manager \[11\] | Event Bus | InstrumentStateChange events | 10 |

**Circuit breakers:**

| From | To | Data | Flow |
| :---- | :---- | :---- | :---- |
| Mark Price Engine \[8\] | Instrument Manager \[11\] | Velocity threshold exceeded → COOLDOWN | 8 |
| Insurance Vault \[12\] | Instrument Manager \[11\] | Utilization \>90% → EMERGENCY | 8 |
| Admin Dashboard \[29\] | Instrument Manager \[11\] | Kill switch → EMERGENCY; recovery → TRADING | 8 |
| Oracle Integration \[22\] | Instrument Manager \[11\] | Oracle halt → ORACLE\_HALT | 9 |
| Instrument Manager \[11\] | Pre-Trade Risk \[3\] | Current instrument state (for order validation) | 2, 8 |

**Custody:**

| From | To | Data | Flow |
| :---- | :---- | :---- | :---- |
| User's wallet | Deposit Wallets \[16\] | USDC transfer (on-chain) | 1 |
| Chain Listener \[20\] | Internal Ledger \[14\] | Deposit detected → credit wallet\_balance | 1 |
| Internal Ledger \[14\] | MPC Infrastructure \[15\] | Withdrawal request (after margin check) | 1 |
| MPC Infrastructure \[15\] | Blockchain | Signed withdrawal tx | 1 |
| Fund Sweeper \[21\] | Hot/Warm/Cold \[17-19\] | Threshold-based fund movement | 1 |
| Reconciliation \[31\] | All wallets \+ Ledger \[14\] | Balance comparison every 5 min | 1 |

### What makes this system work

Three properties that, if violated, mean the exchange is broken:

**1\. Zero-sum invariant.** At every instant: Σ(all realized PnL) \+ Σ(all unrealized PnL) \= 0\. Every dollar one trader gains, another loses. The Internal Ledger \[14\] checks this continuously. Violation \= accounting bug. This is the foundational property of a derivatives exchange.

**2\. Margin coverage.** Every open position has margin reserved. Every resting order has margin reserved. No position exists without collateral backing it. The Risk Engine \[6\] enforces this every mark price tick. The Pre-Trade Risk \[3\] enforces this at order entry. Violation \= undercollateralized positions \= insurance vault drain \= potential insolvency.

**3\. Deterministic ordering.** The Sequencer \[2\] assigns a global sequence to every event. The WAL enables crash recovery by replaying from the last checkpoint. If the matching engine crashes and restarts, it replays the WAL and arrives at exactly the same state. No ambiguity about what happened in what order. Violation \= state divergence after recovery \= potential double-fills or lost orders.

### What the critical path looks like

The order-to-fill hot path — every millisecond of latency here matters:

Client → API Gateway → Sequencer → Pre-Trade Risk → Matching Engine → Trade

  \[1\]        \[1\]          \[2\]           \[3\]              \[4\]

Target: \<10ms total (API Gateway to trade event published)

Components NOT on the critical path:

  • Market Data Engine \[10\] — consumes events asynchronously

  • Risk Engine \[6\] — post-trade check, async

  • Audit Trail \[26\] — async write

  • Surveillance \[27\] — async analysis

  • All custody components — separate flow entirely

The matching engine NEVER waits for any downstream consumer.

It publishes to the event bus and processes the next order.

### What happens when things go wrong — failure scenarios

| Scenario | Detection | Response | Recovery |
| :---- | :---- | :---- | :---- |
| Matching engine crash | Sequencer WAL stops advancing | All order processing stops | Restart, replay WAL from last checkpoint. Deterministic — arrives at same state. |
| Oracle stale (5-30s) | Oracle Integration \[22\] staleness check | DEGRADED: tighten bands, freeze stops | Auto-recover when oracle resumes |
| Oracle down (\>30s) | Oracle Integration \[22\] critical threshold | ORACLE\_HALT: freeze mark, pause liquidation, reduce-only | Admin confirms recovery after oracle resumes |
| Flash crash (\>8% in 60s) | Velocity detection in Mark Price Engine \[8\] | COOLDOWN: block new aggression, allow reduces | Auto-recover after timer (30-60s) |
| Correlated crash (3+ instruments) | Instrument Manager \[11\] counts cooldowns | EMERGENCY: reduce-only all instruments | Manual admin recovery |
| Insurance vault \>90% | Vault utilization monitor | EMERGENCY: system-wide | Manual admin recovery |
| Liquidation cascade | Gradual liquidation \+ impact bands | Liquidations slow down, book absorbs in chunks | Normal operation resumes as positions are closed |
| Vault depleted | Insurance Vault \[12\] balance check | ADL: force-deleverage profitable traders | Vault needs recapitalization. ADL is one-time. |
| Hot wallet empty | Withdrawal request fails | Queue withdrawal, trigger warm→hot sweep | Process queued withdrawals when funded |
| Reconciliation mismatch | Reconciliation Engine \[31\] every 5 min | Alert ops. If persistent: pause withdrawals. | Investigate and resolve discrepancy |
| Settlement oracle failure | Oracle Integration \[22\] during TWAP | Switch to secondary → extend window → admin | See Flow 9 full procedure |

### System startup & recovery sequence

If the entire system goes down and restarts, components must come up in dependency order. A component should not accept traffic until its dependencies are healthy.

Phase 1: Infrastructure (no dependencies)

  □ Database / storage

  □ Event bus (Redis/NATS/Kafka)

  □ MPC infrastructure (Fireblocks)

Phase 2: Data sources (depends on Phase 1\)

  □ Chain Listeners \[20\] — reconnect to all blockchains, replay from last checkpoint

  □ Oracle Integration \[22\] — connect to Pyth \+ Switchboard, validate feeds are live

  □ Internal Ledger \[14\] — load from database, verify integrity

Phase 3: Core engine (depends on Phase 2\)

  □ Instrument Manager \[11\] — load all active instruments, check states, resume timers

  □ Mark Price Engine \[8\] — requires Oracle Integration. Compute current mark prices.

  □ Sequencer \[2\] — load WAL, identify last checkpoint

Phase 4: Trading engine (depends on Phase 3\)

  □ Matching Engine \[4\] — replay WAL from last checkpoint to rebuild orderbook state

  □ Pre-Trade Risk \[3\] — requires Instrument Manager (instrument states) \+ Mark Price (for bands)

  □ OMS \[5\] — rebuild order states from WAL replay, rebuild stop trigger queue

  □ Risk Engine \[6\] — requires Mark Price \+ Internal Ledger. Recalculate all positions.

  □ Liquidation Engine \[7\] — requires Risk Engine. Scan for undercollateralized positions.

  □ Insurance Vault \[12\] — load balance, check utilization

Phase 5: Client-facing (depends on Phase 4\)

  □ Market Data Engine \[10\] — requires Matching Engine events. Build initial snapshots.

  □ WebSocket Fanout Layer — requires Market Data Engine

  □ API Gateway \[1\] — last to come up. Only accept client connections when everything is ready.

  □ REST API — serve from Market Data Engine snapshots

Verification before accepting orders:

  □ WAL replayed to completion — orderbook matches pre-crash state

  □ Mark price is live and within bounds

  □ All instrument states are correct (check if any should have transitioned during downtime)

  □ Reconciliation passes (on-chain balances match internal ledger)

  □ Zero-sum invariant holds

Only after all verifications pass: API Gateway starts accepting orders.

The key property: **the WAL makes the matching engine crash-safe.** Replay the WAL from the last checkpoint and you arrive at exactly the same orderbook state. All other components rebuild their state from the WAL events \+ database. No manual intervention needed for a clean restart. Manual intervention only needed if the verification checks fail (which would indicate data corruption, not just a restart).

### The Asgard FnO architecture in one sentence

A single-threaded sequencer feeds deterministically ordered events through a pre-trade risk check into a CLOB matching engine, which publishes trades to an event bus consumed by a risk engine (continuous margin), liquidation engine (gradual, with insurance vault and ADL backstop), market data engine (L2 depth, trades, ticker, klines via WebSocket fanout), and audit trail — all backed by MPC custody, oracle-derived mark pricing with multi-layer circuit breakers, and a template-driven instrument manager that maintains a rolling calendar of expiry-based futures contracts.

