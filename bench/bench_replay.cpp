// Microbenchmark: events/sec of the order-book engines on a real LOBSTER stream,
// with I/O excluded. Events are parsed into memory first; only the apply loop is
// timed. Both the std::map OrderBook (baseline) and the flat-array FlatBook are
// run, and a per-event checksum of the best quotes is compared so the speedup is
// reported alongside a proof that the faster book reproduces the slower one
// exactly on this data.
//
// Usage: bench <message_file> [iterations]

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "lob/flat_book.hpp"
#include "lob/order_book.hpp"

namespace {

using namespace lob;

struct Event {
    int      type;
    OrderId  id;
    Quantity size;
    Price    price;
    Side     side;
};

bool parse_event(const std::string& line, Event& e) {
    std::string_view sv{line};
    long long        f[6];
    int              n     = 0;
    std::size_t      start = 0;
    while (n < 6) {
        std::size_t comma = sv.find(',', start);
        std::string_view tok =
            sv.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
        if (n != 0) {  // field 0 is the timestamp; unused
            long long  v;
            const auto r = std::from_chars(tok.data(), tok.data() + tok.size(), v);
            if (r.ec != std::errc{})
                return false;
            f[n] = v;
        }
        ++n;
        if (comma == std::string_view::npos)
            break;
        start = comma + 1;
    }
    if (n < 6)
        return false;
    e.type  = static_cast<int>(f[1]);
    e.id    = static_cast<OrderId>(f[2]);
    e.size  = static_cast<Quantity>(f[3]);
    e.price = static_cast<Price>(f[4]);
    e.side  = f[5] == 1 ? Side::Buy : Side::Sell;
    return true;
}

// Fold the inside quote into a running 64-bit hash so any divergence between the
// two engines, at any event, changes the final value.
inline void mix(std::uint64_t& h, std::uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
}

template <class Book>
inline void apply(Book& book, const Event& e) {
    switch (e.type) {
        case 1: book.add_limit(e.id, e.side, e.price, e.size); break;
        case 2:
        case 4: book.reduce(e.id, e.size); break;
        case 3: book.cancel(e.id); break;
        default: break;  // 5 hidden, 6 cross, 7 halt
    }
}

// Speed: apply only, so the timing reflects the data structure, not the
// per-event best-quote queries (which are identical work for both engines).
template <class Book>
double run_timed(Book& book, const std::vector<Event>& events) {
    const auto t0 = std::chrono::steady_clock::now();
    for (const Event& e : events)
        apply(book, e);
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(t1 - t0).count();
}

// Correctness: fold the inside quote after every event so any divergence
// between the two engines, at any event, changes the final value.
template <class Book>
std::uint64_t run_checked(Book& book, const std::vector<Event>& events) {
    std::uint64_t checksum = 0;
    for (const Event& e : events) {
        apply(book, e);
        Price bid, ask;
        mix(checksum, book.best_bid(bid) ? static_cast<std::uint64_t>(bid) : 0xBADBADULL);
        mix(checksum, book.best_ask(ask) ? static_cast<std::uint64_t>(ask) : 0xF00DF00DULL);
    }
    return checksum;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::cerr << "usage: bench <message_file> [iterations]\n";
        return 2;
    }
    const int iters = argc == 3 ? std::max(1, std::atoi(argv[2])) : 5;

    std::ifstream in{argv[1]};
    if (!in) {
        std::cerr << std::format("error: cannot open {}\n", argv[1]);
        return 1;
    }

    std::vector<Event> events;
    Price              lo = std::numeric_limits<Price>::max();
    Price              hi = std::numeric_limits<Price>::min();
    std::string        line;
    while (std::getline(in, line)) {
        if (line.empty())
            continue;
        Event e;
        if (!parse_event(line, e))
            continue;
        if (e.type == 1) {  // new orders define the price band
            lo = std::min(lo, e.price);
            hi = std::max(hi, e.price);
        }
        events.push_back(e);
    }
    if (events.empty() || lo > hi) {
        std::cerr << "error: no usable events\n";
        return 1;
    }
    const Price       base = lo;
    const std::size_t span = static_cast<std::size_t>(hi - lo) + 1;

    std::cout << std::format("{} events loaded; price band [{}, {}] -> {} levels\n", events.size(),
                             lo, hi, span);

    double map_best = 1e30, flat_best = 1e30;
    for (int i = 0; i < iters; ++i) {
        OrderBook ob;
        map_best = std::min(map_best, run_timed(ob, events));
    }
    for (int i = 0; i < iters; ++i) {
        FlatBook fb{base, span};
        flat_best = std::min(flat_best, run_timed(fb, events));
    }

    // Correctness cross-check (untimed): both engines, event for event.
    OrderBook     ob_chk;
    FlatBook      fb_chk{base, span};
    const std::uint64_t map_sum  = run_checked(ob_chk, events);
    const std::uint64_t flat_sum = run_checked(fb_chk, events);
    const std::uint64_t flat_oob = fb_chk.out_of_band();

    const double n      = static_cast<double>(events.size());
    const double map_mps  = n / map_best / 1e6;
    const double flat_mps = n / flat_best / 1e6;

    std::cout << std::format("best of {} runs (apply loop only, I/O excluded):\n", iters);
    std::cout << std::format("  std::map OrderBook : {:8.2f} ms   {:6.2f} M events/s\n",
                             map_best * 1e3, map_mps);
    std::cout << std::format("  flat-array FlatBook: {:8.2f} ms   {:6.2f} M events/s\n",
                             flat_best * 1e3, flat_mps);
    std::cout << std::format("  speedup            : {:5.2f}x\n", map_best / flat_best);

    // On mismatch, pinpoint the first diverging event to aid debugging.
    if (map_sum != flat_sum) {
        OrderBook a;
        FlatBook  b{base, span};
        for (std::size_t i = 0; i < events.size(); ++i) {
            apply(a, events[i]);
            apply(b, events[i]);
            Price ab, aa, bb, ba;
            bool  ahb = a.best_bid(ab), aha = a.best_ask(aa);
            bool  bhb = b.best_bid(bb), bha = b.best_ask(ba);
            if (ahb != bhb || aha != bha || (ahb && ab != bb) || (aha && aa != ba)) {
                const Event& e = events[i];
                std::cerr << std::format(
                    "[DIVERGE] event {} (type={} id={} size={} price={} side={})\n", i, e.type, e.id,
                    e.size, e.price, e.side == Side::Buy ? "buy" : "sell");
                std::cerr << std::format("  map : bid {} ask {}\n", ahb ? ab : -1, aha ? aa : -1);
                std::cerr << std::format("  flat: bid {} ask {}\n", bhb ? bb : -1, bha ? ba : -1);
                break;
            }
        }
    }

    const bool ok = map_sum == flat_sum && flat_oob == 0;
    std::cout << std::format("  best-quote checksums {} (map={:#x} flat={:#x}){}\n",
                             map_sum == flat_sum ? "match" : "DIFFER", map_sum, flat_sum,
                             flat_oob ? std::format(", {} out-of-band!", flat_oob) : std::string{});
    return ok ? 0 : 1;
}
