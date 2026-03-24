#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "../engine/orderbook.hpp"

using namespace asgard;

// Helper: build a simple limit order
static Order make_order(const std::string& id, OrderSide side,
                        double price, double qty, const std::string& user = "U1") {
    Order o;
    o.order_id    = id;
    o.user_id     = user;
    o.instrument  = "BTC-USDC-PERP";
    o.side        = side;
    o.type        = OrderType::LIMIT;
    o.price       = price;
    o.quantity    = qty;
    o.remaining_qty = qty;
    o.tif         = TIF::GTC;
    o.status      = OrderStatus::NEW;
    return o;
}

// ---------------------------------------------------------------------------
// Basic add / best price
// ---------------------------------------------------------------------------

TEST_CASE("OrderBook: best bid and ask after adding orders", "[orderbook]") {
    OrderBook book;

    book.add_order(make_order("B1", OrderSide::BUY,  100.0, 1.0));
    book.add_order(make_order("B2", OrderSide::BUY,  101.0, 1.0));
    book.add_order(make_order("A1", OrderSide::SELL, 102.0, 1.0));
    book.add_order(make_order("A2", OrderSide::SELL, 103.0, 1.0));

    REQUIRE(book.best_bid().has_value());
    REQUIRE(book.best_ask().has_value());
    CHECK(book.best_bid().value() == Catch::Approx(101.0));
    CHECK(book.best_ask().value() == Catch::Approx(102.0));
    CHECK(book.mid_price() == Catch::Approx(101.5));
}

// ---------------------------------------------------------------------------
// FIFO guarantee
// ---------------------------------------------------------------------------

TEST_CASE("OrderBook: FIFO within a price level", "[orderbook]") {
    OrderBook book;
    // Three orders at the same price – should match in insertion order
    book.add_order(make_order("X1", OrderSide::BUY, 100.0, 1.0, "Alice"));
    book.add_order(make_order("X2", OrderSide::BUY, 100.0, 1.0, "Bob"));
    book.add_order(make_order("X3", OrderSide::BUY, 100.0, 1.0, "Carol"));

    // Aggressor is SELL to consume BUY orders → peek_front(SELL, 100.0) returns from bids
    Order* front = book.peek_front(OrderSide::SELL, 100.0);
    REQUIRE(front != nullptr);
    CHECK(front->user_id == "Alice");

    // Consume X1 fully: aggressor is SELL, resting is BUY
    book.consume_front(OrderSide::SELL, 100.0, 1.0);
    // Level still has Bob and Carol, no need to prune

    front = book.peek_front(OrderSide::SELL, 100.0);
    REQUIRE(front != nullptr);
    CHECK(front->user_id == "Bob");
}

// ---------------------------------------------------------------------------
// Cancel / remove
// ---------------------------------------------------------------------------

TEST_CASE("OrderBook: remove_order by order_id", "[orderbook]") {
    OrderBook book;
    book.add_order(make_order("B1", OrderSide::BUY,  100.0, 2.0));
    book.add_order(make_order("B2", OrderSide::BUY,  100.0, 3.0));

    // Remove middle order
    bool removed = book.remove_order("B1", 100.0, OrderSide::BUY);
    CHECK(removed);
    CHECK(book.order_count() == 1);

    // best_bid still exists (B2 still there)
    CHECK(book.best_bid().has_value());
    CHECK(book.best_bid().value() == Catch::Approx(100.0));

    // Removing non-existent order returns false
    CHECK_FALSE(book.remove_order("NOPE", 100.0, OrderSide::BUY));
}

// ---------------------------------------------------------------------------
// available_qty (used by FOK pre-check)
// ---------------------------------------------------------------------------

TEST_CASE("OrderBook: available_qty for FOK pre-check", "[orderbook]") {
    OrderBook book;
    book.add_order(make_order("A1", OrderSide::SELL, 100.0, 2.0));
    book.add_order(make_order("A2", OrderSide::SELL, 101.0, 3.0));
    book.add_order(make_order("A3", OrderSide::SELL, 102.0, 5.0));

    // BUY sweeping up to 101.0 should see A1+A2 = 5.0
    double avail = book.available_qty(OrderSide::BUY, 101.0);
    CHECK(avail == Catch::Approx(5.0));

    // BUY sweeping up to 100.0 should only see A1 = 2.0
    avail = book.available_qty(OrderSide::BUY, 100.0);
    CHECK(avail == Catch::Approx(2.0));

    // BUY sweeping up to 99.0 should see nothing
    avail = book.available_qty(OrderSide::BUY, 99.0);
    CHECK(avail == Catch::Approx(0.0));
}

// ---------------------------------------------------------------------------
// would_cross (POST_ONLY guard)
// ---------------------------------------------------------------------------

TEST_CASE("OrderBook: would_cross for POST_ONLY orders", "[orderbook]") {
    OrderBook book;
    book.add_order(make_order("A1", OrderSide::SELL, 100.0, 1.0));

    // A BUY at 100 would immediately match A1
    Order incoming_buy = make_order("NEW", OrderSide::BUY, 100.0, 1.0);
    incoming_buy.type  = OrderType::POST_ONLY;
    CHECK(book.would_cross(incoming_buy));

    // A BUY at 99 would not cross
    incoming_buy.price = 99.0;
    CHECK_FALSE(book.would_cross(incoming_buy));
}

// ---------------------------------------------------------------------------
// Edge: price level cleanup after full consumption
// ---------------------------------------------------------------------------

TEST_CASE("OrderBook: price level removed after full consumption", "[orderbook]") {
    OrderBook book;
    book.add_order(make_order("B1", OrderSide::BUY, 100.0, 1.0));

    // Aggressor is SELL consuming the BUY; consume_front prunes empty level automatically
    book.consume_front(OrderSide::SELL, 100.0, 1.0);

    CHECK_FALSE(book.best_bid().has_value());
    CHECK(book.order_count() == 0);
}
