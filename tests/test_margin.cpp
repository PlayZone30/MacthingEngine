#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "../engine/margin.hpp"
#include "../engine/models.hpp"

using namespace asgard;

// Helpers
static Instrument make_inst() {
    Instrument inst;
    inst.symbol     = "BTC-USDC-PERP";
    inst.base_imf   = 0.02;
    inst.imf_factor = 0.00003;
    inst.base_mmf   = 0.01;
    inst.mmf_factor = 0.000015;
    inst.maker_fee_rate = 0.0002;
    inst.taker_fee_rate = 0.0005;
    return inst;
}

static UserAccount make_user_no_pos(double balance = 100'000.0) {
    UserAccount u;
    u.user_id        = "U1";
    u.wallet_balance = balance;
    return u;
}

// ---------------------------------------------------------------------------
// IMF formula: max(base_imf, imf_factor × √notional)
// ---------------------------------------------------------------------------

TEST_CASE("Margin: IMF formula — base floor dominates small notional", "[margin]") {
    Instrument inst = make_inst();
    // Notional = 1 * 1000 = 1000 USDC
    // imf_factor * sqrt(1000) = 0.00003 * 31.62 = 0.000949  <  base_imf=0.02
    // So IMF should be base_imf = 0.02
    double imf_val = calc_imf(1000.0, inst);
    CHECK(imf_val == Catch::Approx(0.02));
}

TEST_CASE("Margin: IMF formula — sqrt scaling dominates large notional", "[margin]") {
    Instrument inst = make_inst();
    // Notional = 500 BTC × $50,000 = $25,000,000
    // imf_factor * sqrt(25e6) = 0.00003 * 5000 = 0.15  >  0.02
    double imf_val = calc_imf(25'000'000.0, inst);
    CHECK(imf_val == Catch::Approx(0.15));
}

TEST_CASE("Margin: 1 BTC at $50,000 base IMF = 2%", "[margin]") {
    Instrument inst = make_inst();
    // notional = 50000, sqrt(50000) = 223.6, * 0.00003 = 0.00671 < 0.02
    double imf_val = calc_imf(50'000.0, inst);
    CHECK(imf_val == Catch::Approx(0.02));
    // IM = 0.02 * 50000 = $1000
    double im = calc_im(50'000.0, inst);
    CHECK(im == Catch::Approx(1000.0));
}

// ---------------------------------------------------------------------------
// MMF formula: max(base_mmf, mmf_factor × √notional)
// ---------------------------------------------------------------------------

TEST_CASE("Margin: MMF formula — base floor for small notional", "[margin]") {
    Instrument inst = make_inst();
    double mmf_val = calc_mmf(50'000.0, inst);
    CHECK(mmf_val == Catch::Approx(0.01));
    double mm = calc_mm(50'000.0, inst);
    CHECK(mm == Catch::Approx(500.0)); // 1% of 50000
}

// ---------------------------------------------------------------------------
// Equity and available balance
// ---------------------------------------------------------------------------

TEST_CASE("Margin: equity = wallet + unrealized PnL", "[margin]") {
    Instrument inst = make_inst();
    UserAccount user = make_user_no_pos(10'000.0);

    // Long 1 BTC entered at 40,000, mark now 50,000 → +$10,000 upnl
    Position pos;
    pos.user_id    = "U1";
    pos.instrument = inst.symbol;
    pos.side       = PositionSide::LONG;
    pos.size       = 1.0;
    pos.entry_price = 40'000.0;
    user.positions[inst.symbol] = pos;

    double eq = cross_equity(user, 50'000.0);
    CHECK(eq == Catch::Approx(20'000.0)); // 10000 wallet + 10000 upnl
}

TEST_CASE("Margin: available balance = equity - position_margin - open_order_margin", "[margin]") {
    Instrument inst = make_inst();
    UserAccount user = make_user_no_pos(10'000.0);
    user.open_order_margin = 500.0;

    Position pos;
    pos.side       = PositionSide::LONG;
    pos.size       = 1.0;
    pos.entry_price = 50'000.0;
    pos.instrument  = inst.symbol;
    user.positions[inst.symbol] = pos;

    // Mark = 50,000, equity = 10,000 + 0 upnl = 10,000
    // position_margin = calc_im(50000, inst) = 1000
    // available = 10000 - 1000 - 500 = 8500
    double avail = available_balance(user, 50'000.0, inst);
    CHECK(avail == Catch::Approx(8500.0));
}

// ---------------------------------------------------------------------------
// Bankruptcy price
// ---------------------------------------------------------------------------

TEST_CASE("Margin: bankruptcy price for long position", "[margin]") {
    Instrument inst = make_inst();
    UserAccount user = make_user_no_pos(2'000.0); // just enough for 2% IM on 50k notional

    Position pos;
    pos.side           = PositionSide::LONG;
    pos.size           = 1.0;
    pos.entry_price    = 50'000.0;
    pos.allocated_margin = 2'000.0; // the margin that was locked
    pos.instrument     = inst.symbol;

    // bankruptcy_price = entry - margin/size = 50000 - 2000/1 = 48000
    double bp = bankruptcy_price(pos, pos.allocated_margin);
    CHECK(bp == Catch::Approx(48'000.0));
}

TEST_CASE("Margin: bankruptcy price for short position", "[margin]") {
    Instrument inst = make_inst();
    Position pos;
    pos.side           = PositionSide::SHORT;
    pos.size           = 1.0;
    pos.entry_price    = 50'000.0;
    pos.allocated_margin = 2'000.0;

    // bankruptcy_price = entry + margin/size = 50000 + 2000 = 52000
    double bp = bankruptcy_price(pos, pos.allocated_margin);
    CHECK(bp == Catch::Approx(52'000.0));
}

// ---------------------------------------------------------------------------
// Additional IM for new order (increases position)
// ---------------------------------------------------------------------------

TEST_CASE("Margin: additional IM for order increasing long position", "[margin]") {
    Instrument inst = make_inst();
    UserAccount user = make_user_no_pos(10'000.0);

    Order o;
    o.user_id   = "U1";
    o.instrument = inst.symbol;
    o.side       = OrderSide::BUY;
    o.type       = OrderType::LIMIT;
    o.price      = 50'000.0;
    o.quantity   = 1.0;
    o.remaining_qty = 1.0;
    o.reduce_only = false;

    double mark = 50'000.0;
    double add_im = additional_im_for_order(o, user, inst, mark);
    // notional = 50000, IM = 2% of 50000 = 1000
    CHECK(add_im == Catch::Approx(1000.0));
}

TEST_CASE("Margin: additional IM for reduce-only order is zero", "[margin]") {
    Instrument inst = make_inst();
    UserAccount user = make_user_no_pos(10'000.0);

    // User is long 1 BTC
    Position pos;
    pos.side       = PositionSide::LONG;
    pos.size       = 1.0;
    pos.entry_price = 50'000.0;
    user.positions[inst.symbol] = pos;

    Order o;
    o.user_id    = "U1";
    o.instrument = inst.symbol;
    o.side       = OrderSide::SELL;
    o.type       = OrderType::LIMIT;
    o.price      = 50'000.0;
    o.quantity   = 1.0;
    o.remaining_qty = 1.0;
    o.reduce_only   = true;

    double add_im = additional_im_for_order(o, user, inst, 50'000.0);
    CHECK(add_im == Catch::Approx(0.0));
}
