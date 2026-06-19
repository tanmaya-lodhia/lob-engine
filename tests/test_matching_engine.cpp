#include <doctest/doctest.h>

#include <numeric>

#include "lob/matching_engine.hpp"

using namespace lob;

namespace {
Quantity filled(const std::vector<Fill>& fs) {
    return std::accumulate(fs.begin(), fs.end(), Quantity{0},
                           [](Quantity a, const Fill& f) { return a + f.qty; });
}
}  // namespace

TEST_CASE("a marketable limit crosses and the maker shrinks") {
    MatchingEngine e;
    e.submit_limit(1, Side::Sell, 100, 10);     // resting ask
    auto fills = e.submit_limit(2, Side::Buy, 100, 6);  // lift 6
    REQUIRE(fills.size() == 1);
    CHECK(fills[0].maker == 1);
    CHECK(fills[0].taker == 2);
    CHECK(fills[0].price == 100);
    CHECK(fills[0].qty == 6);
    CHECK(e.qty_at(Side::Sell, 100) == 4);
    Price p;
    CHECK_FALSE(e.best_bid(p));  // nothing rested on the bid
}

TEST_CASE("a buy sweeps several ask levels in price order") {
    MatchingEngine e;
    e.submit_limit(1, Side::Sell, 100, 5);
    e.submit_limit(2, Side::Sell, 101, 5);
    auto fills = e.submit_limit(3, Side::Buy, 101, 8);
    REQUIRE(fills.size() == 2);
    CHECK(fills[0].price == 100);  // cheaper level first
    CHECK(fills[0].qty == 5);
    CHECK(fills[1].price == 101);
    CHECK(fills[1].qty == 3);
    CHECK(e.qty_at(Side::Sell, 101) == 2);
    CHECK(filled(fills) == 8);
}

TEST_CASE("time priority: oldest order at a price fills first") {
    MatchingEngine e;
    e.submit_limit(1, Side::Sell, 100, 5);  // oldest
    e.submit_limit(2, Side::Sell, 100, 5);  // newer
    auto fills = e.submit_market(3, Side::Buy, 7);
    REQUIRE(fills.size() == 2);
    CHECK(fills[0].maker == 1);  // oldest consumed first
    CHECK(fills[0].qty == 5);
    CHECK(fills[1].maker == 2);
    CHECK(fills[1].qty == 2);
    CHECK(e.qty_at(Side::Sell, 100) == 3);
}

TEST_CASE("a non-marketable limit just rests") {
    MatchingEngine e;
    e.submit_limit(1, Side::Sell, 101, 5);
    auto fills = e.submit_limit(2, Side::Buy, 100, 5);  // below the ask
    CHECK(fills.empty());
    Price p;
    REQUIRE(e.best_bid(p));
    CHECK(p == 100);
    CHECK(e.qty_at(Side::Buy, 100) == 5);
}

TEST_CASE("GTC residual rests after a partial fill") {
    MatchingEngine e;
    e.submit_limit(1, Side::Sell, 100, 3);
    auto fills = e.submit_limit(2, Side::Buy, 100, 5, TimeInForce::GTC);
    CHECK(filled(fills) == 3);
    CHECK(e.qty_at(Side::Sell, 100) == 0);
    Price p;
    REQUIRE(e.best_bid(p));            // residual 2 rests as a bid
    CHECK(p == 100);
    CHECK(e.qty_at(Side::Buy, 100) == 2);
}

TEST_CASE("IOC fills what it can and discards the rest") {
    MatchingEngine e;
    e.submit_limit(1, Side::Sell, 100, 3);
    auto fills = e.submit_limit(2, Side::Buy, 100, 5, TimeInForce::IOC);
    CHECK(filled(fills) == 3);
    Price p;
    CHECK_FALSE(e.best_bid(p));  // nothing rested
    CHECK(e.resting_order_count() == 0);
}

TEST_CASE("FOK executes in full or not at all") {
    SUBCASE("enough liquidity -> full fill") {
        MatchingEngine e;
        e.submit_limit(1, Side::Sell, 100, 3);
        e.submit_limit(2, Side::Sell, 101, 3);
        auto fills = e.submit_limit(3, Side::Buy, 101, 5, TimeInForce::FOK);
        CHECK(filled(fills) == 5);
        CHECK(e.qty_at(Side::Sell, 101) == 1);
    }
    SUBCASE("insufficient liquidity -> nothing happens") {
        MatchingEngine e;
        e.submit_limit(1, Side::Sell, 100, 3);
        auto fills = e.submit_limit(2, Side::Buy, 100, 5, TimeInForce::FOK);
        CHECK(fills.empty());
        CHECK(e.qty_at(Side::Sell, 100) == 3);  // maker untouched
        CHECK(e.resting_order_count() == 1);
    }
}

TEST_CASE("market order stops when the book is exhausted") {
    MatchingEngine e;
    e.submit_limit(1, Side::Sell, 100, 3);
    auto fills = e.submit_market(2, Side::Buy, 5);
    CHECK(filled(fills) == 3);  // only 3 available
    Price p;
    CHECK_FALSE(e.best_ask(p));
    CHECK(e.resting_order_count() == 0);  // market residual never rests
}

TEST_CASE("a sell crosses the bid side symmetrically") {
    MatchingEngine e;
    e.submit_limit(1, Side::Buy, 100, 5);
    e.submit_limit(2, Side::Buy, 99, 5);
    auto fills = e.submit_limit(3, Side::Sell, 99, 8);
    REQUIRE(fills.size() == 2);
    CHECK(fills[0].price == 100);  // best (highest) bid first
    CHECK(fills[1].price == 99);
    CHECK(filled(fills) == 8);
    CHECK(e.qty_at(Side::Buy, 99) == 2);
}

TEST_CASE("cancel removes resting liquidity from matching") {
    MatchingEngine e;
    e.submit_limit(1, Side::Sell, 100, 5);
    CHECK(e.cancel(1));
    CHECK_FALSE(e.cancel(1));  // already gone
    auto fills = e.submit_market(2, Side::Buy, 5);
    CHECK(fills.empty());  // nothing left to hit
}

TEST_CASE("quantity is conserved: fills + resting residual == submitted") {
    MatchingEngine e;
    e.submit_limit(1, Side::Sell, 100, 4);
    auto fills = e.submit_limit(2, Side::Buy, 100, 10, TimeInForce::GTC);
    const Quantity rested = e.qty_at(Side::Buy, 100);
    CHECK(filled(fills) + rested == 10);
}
