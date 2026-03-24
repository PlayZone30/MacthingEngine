#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "../engine/models.hpp"
#include "../engine/risk_checks.hpp"
#include "../engine/margin.hpp"

using namespace asgard;

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

static Instrument make_inst() {
    Instrument inst;
    inst.symbol              = "BTC-USDC-PERP";
    inst.state               = InstrumentState::TRADING;
    inst.min_lot             = 0.001;
    inst.max_lot             = 100.0;
    inst.lot_step            = 0.001;
    inst.price_band_pct      = 0.05;
    inst.impact_band_pct     = 0.02;
    inst.base_imf            = 0.02;
    inst.imf_factor          = 0.00003;
    inst.base_mmf            = 0.01;
    inst.mmf_factor          = 0.000015;
    inst.max_position_size   = 100.0;
    inst.rate_limit_retail   = 10;
    inst.rate_limit_algo     = 50;
    inst.rate_limit_mm       = 300;
    inst.maker_fee_rate      = 0.0002;
    inst.taker_fee_rate      = 0.0005;
    return inst;
}

static UserAccount make_user(double balance = 100'000.0) {
    UserAccount u;
    u.user_id         = "U1";
    u.wallet_balance  = balance;
    u.rate_limit      = 100;
    u.messages_this_sec = 0;
    u.user_type       = UserType::RETAIL;
    return u;
}

static Order make_limit_order(OrderSide side, double price, double qty) {
    Order o;
    o.order_id      = "O1";
    o.user_id       = "U1";
    o.instrument    = "BTC-USDC-PERP";
    o.side          = side;
    o.type          = OrderType::LIMIT;
    o.price         = price;
    o.quantity      = qty;
    o.remaining_qty = qty;
    o.tif           = TIF::GTC;
    o.status        = OrderStatus::NEW;
    return o;
}

// ---------------------------------------------------------------------------
// Check 1: instrument state
// ---------------------------------------------------------------------------

TEST_CASE("RiskChecks: instrument state PRE_OPEN rejects orders", "[risk]") {
    Instrument inst  = make_inst();
    inst.state       = InstrumentState::PRE_OPEN;
    UserAccount user = make_user();
    Order o          = make_limit_order(OrderSide::BUY, 50000.0, 0.1);
    RiskEngine re;

    auto r = re.check_instrument_state(inst);
    CHECK_FALSE(r.pass);
}

TEST_CASE("RiskChecks: instrument state TRADING passes", "[risk]") {
    Instrument inst  = make_inst();
    RiskEngine re;
    auto r = re.check_instrument_state(inst);
    CHECK(r.pass);
}

// ---------------------------------------------------------------------------
// Check 2: price band
// ---------------------------------------------------------------------------

TEST_CASE("RiskChecks: limit BUY above price band rejected", "[risk]") {
    Instrument inst   = make_inst();
    RiskEngine re;
    double mark = 50'000.0;
    // 6% above mark — exceeds 5% band
    Order o = make_limit_order(OrderSide::BUY, mark * 1.06, 0.1);
    auto r = re.check_price_band(o, inst, mark);
    CHECK_FALSE(r.pass);
}

TEST_CASE("RiskChecks: limit SELL below price band rejected", "[risk]") {
    Instrument inst   = make_inst();
    RiskEngine re;
    double mark = 50'000.0;
    // 6% below mark
    Order o = make_limit_order(OrderSide::SELL, mark * 0.94, 0.1);
    auto r = re.check_price_band(o, inst, mark);
    CHECK_FALSE(r.pass);
}

TEST_CASE("RiskChecks: limit BUY within band passes", "[risk]") {
    Instrument inst   = make_inst();
    RiskEngine re;
    double mark = 50'000.0;
    Order o = make_limit_order(OrderSide::BUY, mark * 1.04, 0.1);
    auto r = re.check_price_band(o, inst, mark);
    CHECK(r.pass);
}

// ---------------------------------------------------------------------------
// Check 3: size limits
// ---------------------------------------------------------------------------

TEST_CASE("RiskChecks: below min_lot rejected", "[risk]") {
    Instrument inst = make_inst();
    RiskEngine re;
    Order o = make_limit_order(OrderSide::BUY, 50000.0, 0.0005); // below min 0.001
    auto r = re.check_size(o, inst);
    CHECK_FALSE(r.pass);
}

TEST_CASE("RiskChecks: above max_lot rejected", "[risk]") {
    Instrument inst = make_inst();
    RiskEngine re;
    Order o = make_limit_order(OrderSide::BUY, 50000.0, 200.0); // above max 100
    auto r = re.check_size(o, inst);
    CHECK_FALSE(r.pass);
}

TEST_CASE("RiskChecks: valid lot size passes", "[risk]") {
    Instrument inst = make_inst();
    RiskEngine re;
    Order o = make_limit_order(OrderSide::BUY, 50000.0, 1.0);
    auto r = re.check_size(o, inst);
    CHECK(r.pass);
}

// ---------------------------------------------------------------------------
// Check 4: rate limit
// ---------------------------------------------------------------------------

TEST_CASE("RiskChecks: rate limit exceeded rejected", "[risk]") {
    Instrument inst  = make_inst();
    UserAccount user = make_user();
    user.messages_this_sec = user.rate_limit; // exactly at limit
    RiskEngine re;
    auto r = re.check_rate_limit(user);
    CHECK_FALSE(r.pass);
}

TEST_CASE("RiskChecks: rate limit not exceeded passes", "[risk]") {
    UserAccount user = make_user();
    user.messages_this_sec = user.rate_limit - 1;
    RiskEngine re;
    auto r = re.check_rate_limit(user);
    CHECK(r.pass);
}

// ---------------------------------------------------------------------------
// Check 5: position limit
// ---------------------------------------------------------------------------

TEST_CASE("RiskChecks: position limit exceeded rejected", "[risk]") {
    Instrument inst = make_inst();
    // inst.max_position_size = 100
    UserAccount user = make_user();
    // user already has 90 BTC long
    Position pos;
    pos.user_id    = "U1";
    pos.instrument = inst.symbol;
    pos.side       = PositionSide::LONG;
    pos.size       = 90.0;
    pos.entry_price = 50000.0;
    user.positions[inst.symbol] = pos;

    Order o = make_limit_order(OrderSide::BUY, 50000.0, 20.0); // would reach 110 > 100
    RiskEngine re;
    auto r = re.check_position_limit(o, user, inst);
    CHECK_FALSE(r.pass);
}

// ---------------------------------------------------------------------------
// Check 6: reduce-only validation
// ---------------------------------------------------------------------------

TEST_CASE("RiskChecks: reduce-only with no position rejected", "[risk]") {
    Instrument inst = make_inst();
    UserAccount user = make_user(); // no positions

    Order o = make_limit_order(OrderSide::SELL, 50000.0, 1.0);
    o.reduce_only = true;
    RiskEngine re;
    auto r = re.check_reduce_only(o, user);
    CHECK_FALSE(r.pass); // no position to reduce against
}

TEST_CASE("RiskChecks: reduce-only sell on short position rejected (wrong direction)", "[risk]") {
    Instrument inst = make_inst();
    UserAccount user = make_user();
    Position pos;
    pos.user_id    = "U1";
    pos.instrument = inst.symbol;
    pos.side       = PositionSide::SHORT; // short position
    pos.size       = 1.0;
    pos.entry_price = 50000.0;
    user.positions[inst.symbol] = pos;

    Order o = make_limit_order(OrderSide::SELL, 50000.0, 1.0); // SELL on SHORT would increase, not reduce
    o.reduce_only = true;
    RiskEngine re;
    auto r = re.check_reduce_only(o, user);
    CHECK_FALSE(r.pass); // wrong direction
}

TEST_CASE("RiskChecks: reduce-only sell within position passes", "[risk]") {
    Instrument inst = make_inst();
    UserAccount user = make_user();
    Position pos;
    pos.user_id    = "U1";
    pos.instrument = inst.symbol;
    pos.side       = PositionSide::LONG;
    pos.size       = 2.0;
    pos.entry_price = 50000.0;
    user.positions[inst.symbol] = pos;

    Order o = make_limit_order(OrderSide::SELL, 50000.0, 1.0);
    o.reduce_only = true;
    RiskEngine re;
    auto r = re.check_reduce_only(o, user);
    CHECK(r.pass);
}

// ---------------------------------------------------------------------------
// Check 8: COOLDOWN blocks MARKET orders
// ---------------------------------------------------------------------------

TEST_CASE("RiskChecks: MARKET order blocked during COOLDOWN", "[risk]") {
    Instrument inst = make_inst();
    inst.state      = InstrumentState::COOLDOWN;
    Order o = make_limit_order(OrderSide::BUY, 50000.0, 1.0);
    o.type = OrderType::MARKET;
    RiskEngine re;
    auto r = re.check_cooldown_mode(o, inst);
    CHECK_FALSE(r.pass);
}

TEST_CASE("RiskChecks: LIMIT GTC passes during COOLDOWN", "[risk]") {
    Instrument inst = make_inst();
    inst.state      = InstrumentState::COOLDOWN;
    Order o = make_limit_order(OrderSide::BUY, 50000.0, 1.0);
    o.type = OrderType::LIMIT;
    o.tif  = TIF::GTC;
    RiskEngine re;
    auto r = re.check_cooldown_mode(o, inst);
    CHECK(r.pass);
}
