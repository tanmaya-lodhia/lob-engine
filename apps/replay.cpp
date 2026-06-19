// Replays a LOBSTER message file through the reconstruction engine and prints
// summary statistics. This is the driver that maps the LOBSTER event encoding
// onto the engine's add/reduce/cancel primitives.
//
// LOBSTER message file: CSV, one event per row, columns
//   Time, EventType, OrderID, Size, Price, Direction
// EventType:
//   1 = new visible limit order
//   2 = partial cancellation (deletion of `Size` from a resting order)
//   3 = full deletion of a resting order
//   4 = execution of a visible limit order
//   5 = execution of a hidden order        (no visible-book change)
//   6 = cross / auction trade              (ignored here)
//   7 = trading halt indicator             (ignored here)
// Direction: 1 = buy-side order, -1 = sell-side order.
//
// Usage: replay <path-to_message_file.csv>

#include <charconv>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <format>
#include <string>
#include <string_view>

#include "lob/order_book.hpp"

namespace {

using namespace lob;

// Minimal, allocation-free field splitting on a single CSV line. LOBSTER files
// are plain numeric CSV with no quoting, so this is sufficient and fast.
bool parse_long(std::string_view sv, long long& out) {
    auto* first = sv.data();
    auto* last  = sv.data() + sv.size();
    auto  res   = std::from_chars(first, last, out);
    return res.ec == std::errc{} && res.ptr == last;
}

struct Row {
    int       type;
    OrderId   id;
    Quantity  size;
    Price     price;
    int       direction;  // +1 buy, -1 sell
};

bool parse_row(const std::string& line, Row& r) {
    // Split into the six expected fields.
    std::string_view sv{line};
    long long        fields[6];
    int              n = 0;
    std::size_t      start = 0;
    while (n < 6) {
        std::size_t comma = sv.find(',', start);
        std::string_view tok =
            sv.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
        // Time (field 0) is fractional seconds; we don't need it for the book,
        // so only fields 1..5 must parse as integers.
        if (n == 0) {
            // skip parsing the timestamp
        } else if (!parse_long(tok, fields[n])) {
            return false;
        }
        ++n;
        if (comma == std::string_view::npos)
            break;
        start = comma + 1;
    }
    if (n < 6)
        return false;
    r.type      = static_cast<int>(fields[1]);
    r.id        = static_cast<OrderId>(fields[2]);
    r.size      = static_cast<Quantity>(fields[3]);
    r.price     = static_cast<Price>(fields[4]);
    r.direction = static_cast<int>(fields[5]);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: replay <lobster_message_file.csv>\n";
        return 2;
    }
    std::ifstream in{argv[1]};
    if (!in) {
        std::cerr << std::format("error: cannot open {}\n", argv[1]);
        return 1;
    }

    OrderBook book;
    std::string line;
    std::uint64_t rows = 0, adds = 0, cancels = 0, deletes = 0, execs = 0,
                  hidden = 0, other = 0, bad = 0;
    std::int64_t traded_volume = 0;

    while (std::getline(in, line)) {
        if (line.empty())
            continue;
        Row r;
        if (!parse_row(line, r)) {
            ++bad;
            continue;
        }
        ++rows;
        const Side side = r.direction == 1 ? Side::Buy : Side::Sell;
        switch (r.type) {
            case 1: book.add_limit(r.id, side, r.price, r.size); ++adds; break;
            case 2: book.reduce(r.id, r.size); ++cancels; break;
            case 3: book.cancel(r.id); ++deletes; break;
            case 4: book.reduce(r.id, r.size); ++execs; traded_volume += r.size; break;
            case 5: ++hidden; traded_volume += r.size; break;  // hidden: no book change
            default: ++other; break;                            // 6, 7
        }
    }

    Price bid = kNoPrice, ask = kNoPrice;
    const bool have_bid = book.best_bid(bid);
    const bool have_ask = book.best_ask(ask);

    std::cout << std::format("replayed {} events ({} unparseable)\n", rows, bad);
    std::cout << std::format(
        "  adds={}  partial_cancels={}  deletes={}  visible_exec={}  hidden_exec={}  other={}\n",
        adds, cancels, deletes, execs, hidden, other);
    std::cout << std::format("  traded volume (visible+hidden) = {}\n", traded_volume);
    std::cout << std::format("  resting orders at end = {}\n", book.resting_order_count());

    if (have_bid && have_ask) {
        std::cout << std::format("  final best bid/ask = {} / {}  (spread {} ticks)\n",
                                 bid, ask, ask - bid);
    } else {
        std::cout << "  final book has an empty side\n";
    }

    std::cout << "  top 5 bids / asks (price ticks, qty):\n";
    auto bids = book.top_levels(Side::Buy, 5);
    auto asks = book.top_levels(Side::Sell, 5);
    for (std::size_t i = 0; i < 5; ++i) {
        std::string lhs = i < bids.size() ? std::format("{:>8} x {:>6}", bids[i].price, bids[i].qty)
                                          : std::string(17, ' ');
        std::string rhs = i < asks.size() ? std::format("{:>8} x {:>6}", asks[i].price, asks[i].qty)
                                          : std::string{};
        std::cout << std::format("    {}    |    {}\n", lhs, rhs);
    }
    return 0;
}
