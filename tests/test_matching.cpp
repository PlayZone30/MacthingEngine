#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "../engine/models.hpp"
#include "../engine/orderbook.hpp"
#include "../engine/sequencer.hpp"
#include "../engine/matching.hpp"

using namespace asgard;

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

struct MatchingFixture {
    Instrument inst;
    InsuranceVault vault;
    std::unordered_map<std::string, UserAccount> accounts;
    OrderBook   book;
    Sequencer   seq;
    std::unique_ptr<MatchingEngine> engine;
    double mark_price = 50'000.0;

    MatchingFixture() {
        inst.symbol          = "BTC-USDC-PERP";
        inst.state           = InstrumentState::TRADING;
        inst.min_lot         = 0.001;
        inst.max_lot         = 500.0;
        inst.lot_step        = 0.001;
        inst.price_band_pct  = 0.05;
        inst.impact_band_pct = 0.02;
        inst.base_imf        = 0.02;
        inst.imf_factor      = 0.00003;
        inst.base_mmf        = 0.01;
        inst.mmf_factor      = 0.000015;
        inst.maker_fee_rate  = 0.0002;
        inst.taker_fee_rate  = 0.0005;

        engine = std::make_unique<MatchingEngine>(book, seq, inst, accounts);
    }

    UserAccount& add_user(const std::string& id, double balance = 100'000.0) {
        UserAccount acc;
        acc.user_id        = id;
        acc.wallet_balance = balance;
        acc.rate_limit     = 1000;
        acc.user_type      = UserType::RETAIL;
        accounts[id]       = acc;
        return accounts[id];
    }

    Order make_limit(const std::string& order_id, const std::string& user,
                     OrderSide side, double price, double qty,
                     TIF tif = TIF::GTC) {
        Order o;
        o.order_id     = order_id;
        o.user_id      = user;
        o.instrument   = inst.symbol;
        o.side         = side;
        o.type         = OrderType::LIMIT;
        o.price        = price;
        o.quantity     = qty;
        o.remaining_qty = qty;
        o.tif          = tif;
        o.stp_mode     = STPMode::CANCEL_INCOMING;
        o.status       = OrderStatus::NEW;
        o.timestamp_us = now_us();
        return o;
    }

    Order make_market(const std::string& order_id, const std::string& user,
                      OrderSide side, double qty) {
        Order o = make_limit(order_id, user, side, 0.0, qty, TIF::IOC);
        o.type = OrderType::MARKET;
        return o;
    }
};

// ---------------------------------------------------------------------------
// Simple fill
// ---------------------------------------------------------------------------

TEST_CASE("Matching: simple bid-ask fill", "[matching]") {
    MatchingFixture f;
    f.add_user("Alice", 100'000.0);
    f.add_user("Bob",   100'000.0);

    // Alice rests a sell at 50000
    auto sell = f.make_limit("S1", "Alice", OrderSide::SELL, 50000.0, 1.0);
    auto trades = f.engine->process(sell, f.mark_price);
    CHECK(trades.empty()); // no match yet

    // Bob buys at 50000 — should fill
    auto buy = f.make_limit("B1", "Bob", OrderSide::BUY, 50000.0, 1.0);
    trades = f.engine->process(buy, f.mark_price);

    REQUIRE(trades.size() == 1);
    CHECK(trades[0].price    == Catch::Approx(50000.0));
    CHECK(trades[0].quantity == Catch::Approx(1.0));
    CHECK(trades[0].buyer_id  == "Bob");
    CHECK(trades[0].seller_id == "Alice");
    CHECK(trades[0].buyer_is_taker);
}

// ---------------------------------------------------------------------------
// FIFO correctness: two orders at same price, different users
// ---------------------------------------------------------------------------

TEST_CASE("Matching: FIFO fill order within price level", "[matching]") {
    MatchingFixture f;
    f.add_user("Alice", 100'000.0);
    f.add_user("Bob",   100'000.0);
    f.add_user("Carol", 100'000.0);
    f.add_user("Dave",  100'000.0);

    // Alice then Carol rest sells at 50000
    auto s1 = f.make_limit("S1", "Alice", OrderSide::SELL, 50000.0, 1.0);
    auto s2 = f.make_limit("S2", "Carol", OrderSide::SELL, 50000.0, 1.0);
    f.engine->process(s1, f.mark_price);
    f.engine->process(s2, f.mark_price);

    // Dave buys 1 — should match Alice (FIFO)
    auto buy = f.make_limit("B1", "Dave", OrderSide::BUY, 50000.0, 1.0);
    auto trades = f.engine->process(buy, f.mark_price);

    REQUIRE(trades.size() == 1);
    CHECK(trades[0].seller_id == "Alice");
}

// ---------------------------------------------------------------------------
// FOK: cancelled when depth insufficient
// ---------------------------------------------------------------------------

TEST_CASE("Matching: FOK cancelled when insufficient depth", "[matching]") {
    MatchingFixture f;
    f.add_user("Alice", 100'000.0);
    f.add_user("Bob",   100'000.0);

    // Only 0.5 BTC available
    auto sell = f.make_limit("S1", "Alice", OrderSide::SELL, 50000.0, 0.5);
    f.engine->process(sell, f.mark_price);

    // FOK for 1.0 BTC — should be cancelled (no partial fill)
    auto fok = f.make_limit("F1", "Bob", OrderSide::BUY, 50000.0, 1.0, TIF::FOK);
    auto trades = f.engine->process(fok, f.mark_price);

    CHECK(trades.empty());
    // The resting 0.5 should still be there
    CHECK(f.book.best_ask().has_value());
    CHECK(f.book.best_ask().value() == Catch::Approx(50000.0));
}

// ---------------------------------------------------------------------------
// IOC: partial fill, rest cancelled
// ---------------------------------------------------------------------------

TEST_CASE("Matching: IOC partial fill, residual cancelled", "[matching]") {
    MatchingFixture f;
    f.add_user("Alice", 100'000.0);
    f.add_user("Bob",   100'000.0);

    auto sell = f.make_limit("S1", "Alice", OrderSide::SELL, 50000.0, 0.5);
    f.engine->process(sell, f.mark_price);

    auto ioc = f.make_limit("I1", "Bob", OrderSide::BUY, 50000.0, 1.0, TIF::IOC);
    auto trades = f.engine->process(ioc, f.mark_price);

    REQUIRE(trades.size() == 1);
    CHECK(trades[0].quantity == Catch::Approx(0.5));
    // Residual 0.5 cancelled: book should have no ask remaining
    CHECK_FALSE(f.book.best_ask().has_value());
}

// ---------------------------------------------------------------------------
// POST_ONLY: rejected if would cross
// ---------------------------------------------------------------------------

TEST_CASE("Matching: POST_ONLY rejected on cross", "[matching]") {
    MatchingFixture f;
    f.add_user("Alice", 100'000.0);
    f.add_user("Bob",   100'000.0);

    auto sell = f.make_limit("S1", "Alice", OrderSide::SELL, 50000.0, 1.0);
    f.engine->process(sell, f.mark_price);

    auto post = f.make_limit("P1", "Bob", OrderSide::BUY, 50000.0, 1.0);
    post.type = OrderType::POST_ONLY;
    auto trades = f.engine->process(post, f.mark_price);

    CHECK(trades.empty());
    // Resting sell should still be in book
    CHECK(f.book.best_ask().has_value());
}

// ---------------------------------------------------------------------------
// STP: CANCEL_INCOMING
// ---------------------------------------------------------------------------

TEST_CASE("Matching: STP CANCEL_INCOMING prevents self-trade", "[matching]") {
    MatchingFixture f;
    f.add_user("Alice", 100'000.0);

    // Alice rests a sell
    auto sell = f.make_limit("S1", "Alice", OrderSide::SELL, 50000.0, 1.0);
    f.engine->process(sell, f.mark_price);

    // Alice tries to buy (would self-trade) — incoming is cancelled
    auto buy = f.make_limit("B1", "Alice", OrderSide::BUY, 50000.0, 1.0);
    buy.stp_mode = STPMode::CANCEL_INCOMING;
    auto trades = f.engine->process(buy, f.mark_price);

    CHECK(trades.empty());
    // Resting sell should still be there
    CHECK(f.book.best_ask().has_value());
}

// ---------------------------------------------------------------------------
// Cancel resting order
// ---------------------------------------------------------------------------

TEST_CASE("Matching: cancel resting order", "[matching]") {
    MatchingFixture f;
    auto& alice = f.add_user("Alice", 100'000.0);

    auto sell = f.make_limit("S1", "Alice", OrderSide::SELL, 50000.0, 1.0);
    f.engine->process(sell, f.mark_price);
    CHECK(f.book.best_ask().has_value());

    bool cancelled = f.engine->cancel("S1", alice, f.inst.symbol);
    CHECK(cancelled);
    CHECK_FALSE(f.book.best_ask().has_value());
}

// ---------------------------------------------------------------------------
// Partial fill leaves residual resting
// ---------------------------------------------------------------------------

TEST_CASE("Matching: partial fill of resting order", "[matching]") {
    MatchingFixture f;
    f.add_user("Alice", 100'000.0);
    f.add_user("Bob",   100'000.0);

    auto sell = f.make_limit("S1", "Alice", OrderSide::SELL, 50000.0, 2.0);
    f.engine->process(sell, f.mark_price);

    auto buy = f.make_limit("B1", "Bob", OrderSide::BUY, 50000.0, 1.0);
    auto trades = f.engine->process(buy, f.mark_price);

    REQUIRE(trades.size() == 1);
    CHECK(trades[0].quantity == Catch::Approx(1.0));

    // 1 BTC should remain resting on the ask side
    CHECK(f.book.best_ask().has_value());
    // peek_front takes aggressor side: BUY aggressor → returns from asks
    Order* resting = f.book.peek_front(OrderSide::BUY, 50000.0);
    REQUIRE(resting != nullptr);
    CHECK(resting->remaining_qty == Catch::Approx(1.0));
}
