// A tiny scripted demonstration of the matching engine: build a book, send a
// few aggressive orders, and print the fills and resulting top of book. This is
// illustrative; the engine's behaviour is pinned by tests/test_matching_engine.

#include <iostream>
#include <format>
#include <string_view>

#include "lob/matching_engine.hpp"

using namespace lob;

namespace {

std::string_view tif_name(TimeInForce t) {
    switch (t) {
        case TimeInForce::GTC: return "GTC";
        case TimeInForce::IOC: return "IOC";
        case TimeInForce::FOK: return "FOK";
    }
    return "?";
}

void print_fills(std::string_view label, const std::vector<Fill>& fills) {
    std::cout << label << ": ";
    if (fills.empty()) {
        std::cout << "(no fills)\n";
        return;
    }
    Quantity total = 0;
    for (const auto& f : fills) {
        std::cout << std::format("[maker {} {}@{}] ", f.maker, f.qty, f.price);
        total += f.qty;
    }
    std::cout << std::format("=> {} filled\n", total);
}

void print_book(const MatchingEngine& e) {
    auto bids = e.top_levels(Side::Buy, 3);
    auto asks = e.top_levels(Side::Sell, 3);
    std::cout << "  book  bids:";
    for (auto& l : bids)
        std::cout << std::format(" {}x{}", l.price, l.qty);
    std::cout << "   asks:";
    for (auto& l : asks)
        std::cout << std::format(" {}x{}", l.price, l.qty);
    std::cout << "\n";
}

}  // namespace

int main() {
    MatchingEngine e;

    std::cout << "seed the book with resting liquidity\n";
    e.submit_limit(1, Side::Sell, 10100, 200);
    e.submit_limit(2, Side::Sell, 10100, 100);  // behind #1 at the same price
    e.submit_limit(3, Side::Sell, 10200, 300);
    e.submit_limit(4, Side::Buy, 10000, 150);
    e.submit_limit(5, Side::Buy, 9900, 400);
    print_book(e);

    std::cout << "\naggressive buy limit 250 @ 10100 (lifts the inside ask, time priority)\n";
    print_fills("fills", e.submit_limit(6, Side::Buy, 10100, 250));
    print_book(e);

    std::cout << "\nmarket sell 500 (sweeps the bids, stops when exhausted-aware)\n";
    print_fills("fills", e.submit_market(7, Side::Sell, 500));
    print_book(e);

    std::cout << std::format("\nfill-or-kill buy 1000 @ 10200 ({}) -- not enough, killed\n",
                             tif_name(TimeInForce::FOK));
    print_fills("fills", e.submit_limit(8, Side::Buy, 10200, 1000, TimeInForce::FOK));
    print_book(e);

    return 0;
}
