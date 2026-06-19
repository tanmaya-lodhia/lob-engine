// Invariant tests for FlatBook. The exhaustive correctness check is in the
// benchmark, which replays the real LOBSTER stream through both FlatBook and the
// validated OrderBook and asserts identical best quotes at every event; these
// cases pin the behaviour directly and document intent.
#include <doctest/doctest.h>

#include <random>

#include "lob/flat_book.hpp"
#include "lob/order_book.hpp"

using namespace lob;

namespace {
// A band wide enough for the prices used below.
FlatBook make_book() { return FlatBook(/*base=*/9000, /*span=*/2000); }

Price best_bid_or(const FlatBook& b, Price fallback) {
    Price p;
    return b.best_bid(p) ? p : fallback;
}
Price best_ask_or(const FlatBook& b, Price fallback) {
    Price p;
    return b.best_ask(p) ? p : fallback;
}
}  // namespace

TEST_CASE("flat: best bid is the highest, best ask the lowest") {
    auto b = make_book();
    b.add_limit(1, Side::Buy, 9900, 100);
    b.add_limit(2, Side::Buy, 9901, 50);
    b.add_limit(3, Side::Sell, 9905, 200);
    b.add_limit(4, Side::Sell, 9903, 75);
    CHECK(best_bid_or(b, -1) == 9901);
    CHECK(best_ask_or(b, -1) == 9903);
    CHECK(b.qty_at(Side::Buy, 9900) == 100);
    CHECK(b.resting_order_count() == 4);
}

TEST_CASE("flat: orders at one price aggregate and report in top_levels") {
    auto b = make_book();
    b.add_limit(1, Side::Buy, 9900, 100);
    b.add_limit(2, Side::Buy, 9900, 250);
    CHECK(b.qty_at(Side::Buy, 9900) == 350);
    auto top = b.top_levels(Side::Buy, 5);
    REQUIRE(top.size() == 1);
    CHECK(top[0].price == 9900);
    CHECK(top[0].qty == 350);
}

TEST_CASE("flat: best advances to the next level when the inside empties") {
    auto b = make_book();
    b.add_limit(1, Side::Sell, 9903, 75);
    b.add_limit(2, Side::Sell, 9905, 200);
    CHECK(best_ask_or(b, -1) == 9903);
    b.cancel(1);  // remove the inside ask
    CHECK(best_ask_or(b, -1) == 9905);

    b.add_limit(3, Side::Buy, 9900, 100);
    b.add_limit(4, Side::Buy, 9898, 100);
    CHECK(best_bid_or(b, -1) == 9900);
    b.cancel(3);
    CHECK(best_bid_or(b, -1) == 9898);
}

TEST_CASE("flat: reduce shrinks then removes; level and best update") {
    auto b = make_book();
    b.add_limit(1, Side::Sell, 9905, 300);
    b.reduce(1, 100);
    CHECK(b.qty_at(Side::Sell, 9905) == 200);
    b.reduce(1, 999);  // over-reduce removes the order
    CHECK(b.qty_at(Side::Sell, 9905) == 0);
    Price p;
    CHECK_FALSE(b.best_ask(p));
}

TEST_CASE("flat: reduce_at consumes the FIFO front first") {
    auto b = make_book();
    b.add_limit(1, Side::Sell, 9905, 100);  // oldest
    b.add_limit(2, Side::Sell, 9905, 100);
    b.reduce_at(Side::Sell, 9905, 150);
    CHECK(b.qty_at(Side::Sell, 9905) == 50);
    CHECK_FALSE(b.has_order(1));
    CHECK(b.has_order(2));
}

TEST_CASE("flat: unknown ids tolerated; prices outside band counted") {
    auto b = make_book();
    b.add_limit(1, Side::Buy, 9900, 100);
    b.reduce(999, 50);
    b.cancel(888);
    CHECK(b.qty_at(Side::Buy, 9900) == 100);

    b.add_limit(2, Side::Buy, 50, 100);      // below band
    b.add_limit(3, Side::Buy, 100000, 100);  // above band
    CHECK(b.out_of_band() == 2);
    CHECK(b.resting_order_count() == 1);
}

namespace {
// Compare the inside, depth, and top-of-book of the two engines.
bool same_state(const OrderBook& a, const FlatBook& b) {
    Price ab, aa, bb, ba;
    const bool ahb = a.best_bid(ab), aha = a.best_ask(aa);
    const bool bhb = b.best_bid(bb), bha = b.best_ask(ba);
    if (ahb != bhb || aha != bha)
        return false;
    if (ahb && ab != bb)
        return false;
    if (aha && aa != ba)
        return false;
    if (a.resting_order_count() != b.resting_order_count())
        return false;
    for (Side s : {Side::Buy, Side::Sell}) {
        auto la = a.top_levels(s, 8);
        auto lb = b.top_levels(s, 8);
        if (la.size() != lb.size())
            return false;
        for (std::size_t i = 0; i < la.size(); ++i)
            if (la[i].price != lb[i].price || la[i].qty != lb[i].qty)
                return false;
    }
    return true;
}
}  // namespace

TEST_CASE("flat: differential fuzz against OrderBook (forces crossed books)") {
    // Bids and asks are drawn from the same narrow price range, so the stream
    // routinely produces transiently crossed/locked books -- the case a single
    // shared price array gets wrong. Each event is drawn once and applied to
    // both engines, which must agree at every step. Deterministic seed.
    OrderBook ob;
    FlatBook  fb{/*base=*/1000, /*span=*/200};
    std::mt19937                       rng{12345};
    std::uniform_int_distribution<int> price{1000, 1199};  // overlapping ranges
    std::uniform_int_distribution<int> qty{1, 50};
    std::uniform_int_distribution<int> op{0, 9};
    std::uniform_int_distribution<int> coin{0, 1};

    OrderId next = 1;
    for (int step = 0; step < 50000; ++step) {
        const int o = op(rng);
        if (o < 6 || next == 1) {  // ~60% adds (and always until ids exist)
            const OrderId  id = next++;
            const Side     side = coin(rng) ? Side::Buy : Side::Sell;
            const Price    p    = price(rng);
            const Quantity q    = qty(rng);
            ob.add_limit(id, side, p, q);
            fb.add_limit(id, side, p, q);
        } else if (o < 8) {  // partial reduce of some (possibly dead) id
            const OrderId id = 1 + (rng() % static_cast<std::uint64_t>(next - 1));
            const Quantity d = qty(rng);
            ob.reduce(id, d);
            fb.reduce(id, d);
        } else {  // cancel some (possibly dead) id
            const OrderId id = 1 + (rng() % static_cast<std::uint64_t>(next - 1));
            ob.cancel(id);
            fb.cancel(id);
        }
        REQUIRE_MESSAGE(same_state(ob, fb), "diverged at step ", step);
    }
}
