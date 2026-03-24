#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <chrono>

namespace asgard {

// ---------------------------------------------------------------------------
// Utility
// ---------------------------------------------------------------------------

inline uint64_t now_us() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(
            system_clock::now().time_since_epoch()).count());
}

// ---------------------------------------------------------------------------
// Enumerations
// ---------------------------------------------------------------------------

enum class OrderType : uint8_t {
    LIMIT,
    MARKET,
    STOP_LIMIT,
    POST_ONLY
};

enum class OrderSide : uint8_t {
    BUY,
    SELL
};

enum class TIF : uint8_t {      // Time-In-Force
    GTC,                        // Good-till-cancelled
    IOC,                        // Immediate-or-cancel
    FOK                         // Fill-or-kill
};

enum class STPMode : uint8_t {  // Self-Trade Prevention
    CANCEL_INCOMING,
    CANCEL_RESTING,
    CANCEL_BOTH
};

enum class OrderStatus : uint8_t {
    NEW,
    OPEN,
    PARTIAL,
    FILLED,
    CANCELLED,
    REJECTED,
    EXPIRED
};

enum class InstrumentState : uint8_t {
    PRE_OPEN,
    TRADING,
    COOLDOWN,
    CLOSE_ONLY,
    SETTLING,
    SETTLED,
    EMERGENCY
};

enum class MarginMode : uint8_t {
    CROSS,
    ISOLATED
};

enum class PositionSide : uint8_t {
    LONG,
    SHORT
};

enum class PositionState : uint8_t {
    OPEN,
    LIQUIDATING,
    CLOSED
};

enum class UserType : uint8_t {
    RETAIL,
    ALGO,
    MARKET_MAKER
};

enum class TriggerSource : uint8_t {
    MARK,
    LAST_TRADED,
    INDEX
};

// ---------------------------------------------------------------------------
// Order
// ---------------------------------------------------------------------------

struct Order {
    std::string   order_id;
    std::string   user_id;
    std::string   instrument;
    OrderSide     side           = OrderSide::BUY;
    OrderType     type           = OrderType::LIMIT;
    double        price          = 0.0;   // 0 for MARKET orders
    double        quantity       = 0.0;
    double        remaining_qty  = 0.0;
    TIF           tif            = TIF::GTC;
    bool          reduce_only    = false;
    STPMode       stp_mode       = STPMode::CANCEL_INCOMING;
    MarginMode    margin_mode    = MarginMode::CROSS;
    uint64_t      seq            = 0;
    uint64_t      timestamp_us   = 0;
    OrderStatus   status         = OrderStatus::NEW;
    double        arrival_best   = 0.0;   // best price on opposite side at arrival (Layer 2)
    // Stop-limit specific
    double        trigger_price  = 0.0;
    TriggerSource trigger_source = TriggerSource::MARK;
    double        limit_price    = 0.0;   // limit price when stop triggers
};

// ---------------------------------------------------------------------------
// Trade  (emitted by the matching engine)
// ---------------------------------------------------------------------------

struct Trade {
    std::string  trade_id;
    std::string  instrument;
    double       price          = 0.0;
    double       quantity       = 0.0;
    std::string  buyer_id;
    std::string  seller_id;
    std::string  buyer_order_id;
    std::string  seller_order_id;
    bool         buyer_is_taker = true;
    uint64_t     timestamp_us   = 0;
    uint64_t     seq            = 0;
};

// ---------------------------------------------------------------------------
// Position
// ---------------------------------------------------------------------------

struct Position {
    std::string   user_id;
    std::string   instrument;
    PositionSide  side              = PositionSide::LONG;
    double        size              = 0.0;
    double        entry_price       = 0.0;
    MarginMode    margin_mode       = MarginMode::CROSS;
    double        allocated_margin  = 0.0;  // isolated mode only
    PositionState state             = PositionState::OPEN;
};

// ---------------------------------------------------------------------------
// Instrument configuration
// ---------------------------------------------------------------------------

struct Instrument {
    std::string     symbol;
    InstrumentState state               = InstrumentState::TRADING;

    // Lot parameters
    double  min_lot             = 0.001;
    double  max_lot             = 1000.0;
    double  lot_step            = 0.001;

    // Circuit breaker parameters
    double  price_band_pct      = 0.05;   // Layer 1: ±5% around mark
    double  impact_band_pct     = 0.02;   // Layer 2: max 2% sweep
    double  velocity_threshold  = 0.08;   // Layer 3: 8% move triggers cooldown
    int     velocity_window_s   = 60;     // 60-second window
    int     cooldown_duration_s = 30;     // 30-second cooldown

    // Margin parameters (sqrt-based)
    double  base_imf    = 0.02;       // 2% base initial margin fraction
    double  imf_factor  = 0.00003;    // sqrt scaling factor
    double  base_mmf    = 0.01;       // 1% base maintenance margin fraction
    double  mmf_factor  = 0.000015;   // half of imf_factor

    // Position limits
    double  max_position_size = 500.0;  // max 500 BTC equivalent

    // Rate limits by user type (messages/sec)
    int  rate_limit_retail = 10;
    int  rate_limit_algo   = 50;
    int  rate_limit_mm     = 300;

    // Liquidation
    double  liq_chunk_pct   = 0.10;   // 10% per iteration
    double  liq_band_pct    = 0.05;   // 5% from mark for liq order price band
    double  liq_fee_rate    = 0.01;   // 1% liquidation fee

    // Trading fees
    double  maker_fee_rate  = 0.0002; // 0.02%
    double  taker_fee_rate  = 0.0005; // 0.05%
};

// ---------------------------------------------------------------------------
// UserAccount  (only ever written by the engine thread)
// ---------------------------------------------------------------------------

struct UserAccount {
    std::string   user_id;
    UserType      user_type         = UserType::RETAIL;
    double        wallet_balance    = 0.0;
    double        open_order_margin = 0.0;
    double        total_deposited   = 0.0;

    std::unordered_map<std::string, Position>  positions;    // instrument → Position
    std::unordered_map<std::string, Order>     open_orders;  // order_id → Order

    int     rate_limit          = 10;
    int     messages_this_sec   = 0;   // reset every second by engine
    int64_t rate_window_start_s = 0;   // epoch second when window started
};

// ---------------------------------------------------------------------------
// InsuranceVault
// ---------------------------------------------------------------------------

struct InsuranceVault {
    double current_balance      = 200'000.0;  // initial seed
    double total_losses_absorbed = 0.0;
    double sitg_balance         = 100'000.0;  // exchange's permanently locked SITG
    double fee_revenue          = 0.0;        // exchange fee revenue

    double utilization() const {
        double original = current_balance + total_losses_absorbed;
        if (original <= 0.0) return 0.0;
        return total_losses_absorbed / original;
    }

    // Returns false if vault cannot cover the deficit (depleted)
    bool absorb_deficit(double deficit) {
        if (deficit <= 0.0) return true;
        if (deficit > current_balance) {
            total_losses_absorbed += current_balance;
            current_balance = 0.0;
            return false; // vault depleted → ADL needed
        }
        current_balance       -= deficit;
        total_losses_absorbed += deficit;
        return true;
    }

    void credit_fee(double amount) {
        current_balance += amount;
        fee_revenue     += amount;
    }
};

// ---------------------------------------------------------------------------
// Stats snapshot (pushed to subscribers)
// ---------------------------------------------------------------------------

struct StatsSnapshot {
    double   orders_per_sec    = 0.0;
    double   trades_per_sec    = 0.0;
    double   fill_rate_pct     = 0.0;
    uint64_t total_orders      = 0;
    uint64_t total_trades      = 0;
    uint64_t total_rejections  = 0;
    uint64_t total_liquidations = 0;
    double   vault_balance     = 0.0;
    double   vault_utilization = 0.0;
    int      cooldown_triggers  = 0;
    int      emergency_triggers = 0;
    double   best_bid          = 0.0;
    double   best_ask          = 0.0;
    double   mark_price        = 0.0;
    double   zero_sum_check    = 0.0;  // should be ~0
};

} // namespace asgard
