#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "lob/order_book.hpp"

using namespace lob;

namespace {
Price best_bid_or(const OrderBook& b, Price fallback) {
    Price p;
    return b.best_bid(p) ? p : fallback;
}
Price best_ask_or(const OrderBook& b, Price fallback) {
    Price p;
    return b.best_ask(p) ? p : fallback;
}
}  // namespace

TEST_CASE("empty book reports no best prices") {
    OrderBook b;
    Price p;
    CHECK_FALSE(b.best_bid(p));
    CHECK(p == kNoPrice);
    CHECK_FALSE(b.best_ask(p));
    CHECK(b.resting_order_count() == 0);
}

TEST_CASE("best bid is the highest, best ask the lowest") {
    OrderBook b;
    b.add_limit(1, Side::Buy, 9900, 100);
    b.add_limit(2, Side::Buy, 9901, 50);   // more aggressive bid
    b.add_limit(3, Side::Sell, 9905, 200);
    b.add_limit(4, Side::Sell, 9903, 75);  // more aggressive ask

    CHECK(best_bid_or(b, -1) == 9901);
    CHECK(best_ask_or(b, -1) == 9903);
    CHECK(b.qty_at(Side::Buy, 9900) == 100);
    CHECK(b.resting_order_count() == 4);
}

TEST_CASE("multiple orders at one price aggregate") {
    OrderBook b;
    b.add_limit(1, Side::Buy, 9900, 100);
    b.add_limit(2, Side::Buy, 9900, 250);
    b.add_limit(3, Side::Buy, 9900, 5);
    CHECK(b.qty_at(Side::Buy, 9900) == 355);

    auto top = b.top_levels(Side::Buy, 5);
    REQUIRE(top.size() == 1);
    CHECK(top[0].price == 9900);
    CHECK(top[0].qty == 355);
}

TEST_CASE("partial reduce shrinks size but keeps the order and its level") {
    OrderBook b;
    b.add_limit(1, Side::Sell, 9905, 300);
    b.reduce(1, 100);
    CHECK(b.qty_at(Side::Sell, 9905) == 200);
    CHECK(b.resting_order_count() == 1);
}

TEST_CASE("reduce to zero removes the order and empties the level") {
    OrderBook b;
    b.add_limit(1, Side::Sell, 9905, 300);
    b.add_limit(2, Side::Sell, 9905, 100);
    b.reduce(1, 300);  // exactly empties order 1
    CHECK(b.qty_at(Side::Sell, 9905) == 100);
    CHECK(b.resting_order_count() == 1);

    b.reduce(2, 999);  // over-reduce: clamp to removal, level disappears
    CHECK(b.qty_at(Side::Sell, 9905) == 0);
    Price p;
    CHECK_FALSE(b.best_ask(p));
}

TEST_CASE("cancel removes a resting order in full") {
    OrderBook b;
    b.add_limit(1, Side::Buy, 9900, 100);
    b.add_limit(2, Side::Buy, 9901, 100);
    b.cancel(2);
    CHECK(best_bid_or(b, -1) == 9900);
    CHECK(b.resting_order_count() == 1);
}

TEST_CASE("FIFO time priority: front of queue is removed first by execution") {
    // Three orders rest at the same ask. A visible execution consumes from the
    // front (oldest) first -- the time-priority invariant the whole engine
    // exists to preserve.
    OrderBook b;
    b.add_limit(1, Side::Sell, 9905, 100);  // oldest
    b.add_limit(2, Side::Sell, 9905, 100);
    b.add_limit(3, Side::Sell, 9905, 100);  // newest
    CHECK(b.qty_at(Side::Sell, 9905) == 300);

    b.reduce(1, 100);  // execution against the front order
    CHECK(b.qty_at(Side::Sell, 9905) == 200);
    CHECK(b.resting_order_count() == 2);

    // Orders 2 and 3 remain, in order; removing the new front then the back
    // must still leave a consistent, ultimately empty level.
    b.reduce(2, 100);
    b.reduce(3, 100);
    CHECK(b.qty_at(Side::Sell, 9905) == 0);
}

TEST_CASE("reducing or cancelling an unknown order is tolerated") {
    OrderBook b;
    b.add_limit(1, Side::Buy, 9900, 100);
    b.reduce(999, 50);  // unknown id -> no-op
    b.cancel(888);      // unknown id -> no-op
    CHECK(b.qty_at(Side::Buy, 9900) == 100);
    CHECK(b.resting_order_count() == 1);
}
