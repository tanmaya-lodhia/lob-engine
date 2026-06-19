// Oracle test: replay a LOBSTER message file and assert the reconstructed book
// matches LOBSTER's own published orderbook file, row by row, for every event.
//
// This is the strongest correctness check available: LOBSTER publishes the book
// state after every message, so we validate against real exchange-derived data
// rather than only hand-written cases.
//
// Two facts about LOBSTER data drive the design:
//
//  1. The window opens at 09:30 with liquidity already resting. Those orders
//     were never "added" by a message in our file, so we SEED the book from the
//     first orderbook snapshot (one synthetic aggregate order per level) before
//     replaying. We then validate rows 1..N-1.
//
//  2. Cancel/execute messages that touch pre-window liquidity reference an order
//     id we never saw, but they always carry price + size. So we use the precise
//     by-id path when the order is known (the common in-window case) and fall
//     back to price-level reduction (reduce_at) when it is not.
//
// The orderbook file has 4*L columns per row:
//   ask_price_1, ask_size_1, bid_price_1, bid_size_1, ask_price_2, ...
// Missing levels are padded with a dummy price and size 0; we treat size 0 as
// "no level at this rank".
//
// Usage: oracle <message_file> <orderbook_file> <seed_levels> [validate_levels]
//
// seed_levels    how many levels of the first snapshot to seed and the column
//                width of the orderbook file.
// validate_levels (optional, default = seed_levels) how many top levels to
//                check each snapshot. Seeding deeper than you validate buffers
//                the top of book against the feed's depth loss, so e.g.
//                `... 50 1` measures top-of-book fidelity from 50-level data.

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "lob/order_book.hpp"

namespace {

using namespace lob;

// Parse all comma-separated integer fields of a line into `out` (reused buffer).
// Fractional fields (the timestamp) are parsed up to the decimal point, which is
// all we need since we never use the timestamp here.
bool parse_ints(std::string_view sv, std::vector<long long>& out) {
    out.clear();
    std::size_t start = 0;
    while (start <= sv.size()) {
        std::size_t comma = sv.find(',', start);
        std::string_view tok =
            sv.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
        long long  v   = 0;
        const auto res = std::from_chars(tok.data(), tok.data() + tok.size(), v);
        if (res.ec != std::errc{} && res.ptr == tok.data())
            return false;  // not even a leading integer
        out.push_back(v);
        if (comma == std::string_view::npos)
            break;
        start = comma + 1;
    }
    return true;
}

struct Mismatch {
    std::uint64_t row;
    std::string   detail;
};

// Does the engine's level at `rank` equal the oracle's? Size 0 in the oracle
// means "no level at this rank" (LOBSTER pads missing levels), which must align
// with the engine having no level there either.
bool rank_matches(const std::vector<OrderBook::PriceLevel>& engine,
                  const std::vector<std::pair<long long, long long>>& oracle, std::size_t rank) {
    const auto [oprice, osize] = oracle[rank];
    if (osize <= 0)
        return rank >= engine.size();
    return rank < engine.size() && engine[rank].price == oprice && engine[rank].qty == osize;
}

// Number of consecutive ranks, counting from the top of book, at which BOTH
// sides match. A return of `levels` means the full snapshot matched.
int leading_match_depth(const std::vector<OrderBook::PriceLevel>& eng_ask,
                        const std::vector<std::pair<long long, long long>>& ora_ask,
                        const std::vector<OrderBook::PriceLevel>& eng_bid,
                        const std::vector<std::pair<long long, long long>>& ora_bid, int levels) {
    for (int k = 0; k < levels; ++k)
        if (!rank_matches(eng_ask, ora_ask, k) || !rank_matches(eng_bid, ora_bid, k))
            return k;
    return levels;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4 && argc != 5) {
        std::cerr << "usage: oracle <message_file> <orderbook_file> <seed_levels> [validate_levels]\n";
        return 2;
    }
    const int seed_levels = std::atoi(argv[3]);
    const int levels      = argc == 5 ? std::atoi(argv[4]) : seed_levels;
    if (seed_levels <= 0 || levels <= 0 || levels > seed_levels) {
        std::cerr << "require 0 < validate_levels <= seed_levels\n";
        return 2;
    }
    std::ifstream msg{argv[1]};
    std::ifstream ob{argv[2]};
    if (!msg || !ob) {
        std::cerr << "error: cannot open input files\n";
        return 1;
    }

    OrderBook book;
    std::string mline, oline;
    std::vector<long long> mf, of;

    // --- seed from the first orderbook snapshot -----------------------------
    if (!std::getline(msg, mline) || !std::getline(ob, oline)) {
        std::cerr << "error: empty input\n";
        return 1;
    }
    if (!parse_ints(oline, of) || static_cast<int>(of.size()) < 4 * seed_levels) {
        std::cerr << "error: malformed first orderbook row\n";
        return 1;
    }
    OrderId synth = -1;
    for (int k = 0; k < seed_levels; ++k) {
        const long long ap = of[4 * k + 0], as = of[4 * k + 1];
        const long long bp = of[4 * k + 2], bs = of[4 * k + 3];
        if (as > 0)
            book.add_limit(synth--, Side::Sell, ap, as);
        if (bs > 0)
            book.add_limit(synth--, Side::Buy, bp, bs);
    }

    // --- replay and validate rows 1..N-1 ------------------------------------
    // match_at_depth[d] counts rows whose top (d+1) levels all match. The
    // top of book (d=0) is exactly reconstructable; deeper levels degrade
    // because a level-N feed omits events below level N.
    std::uint64_t rows = 0, full_match = 0, applied = 0, first_l1_break = 0;
    std::vector<std::uint64_t> match_at_depth(levels, 0);
    std::vector<Mismatch> first_mismatches;
    std::vector<std::pair<long long, long long>> oracle_ask(levels), oracle_bid(levels);

    const auto t0 = std::chrono::steady_clock::now();
    while (std::getline(msg, mline) && std::getline(ob, oline)) {
        if (mline.empty() || oline.empty())
            continue;
        if (!parse_ints(mline, mf) || mf.size() < 6) {
            continue;
        }
        // message fields: time, type, id, size, price, direction
        const int      type = static_cast<int>(mf[1]);
        const OrderId  id   = static_cast<OrderId>(mf[2]);
        const Quantity size = static_cast<Quantity>(mf[3]);
        const Price    price= static_cast<Price>(mf[4]);
        const Side     side = mf[5] == 1 ? Side::Buy : Side::Sell;

        switch (type) {
            case 1: book.add_limit(id, side, price, size); break;
            case 2:
            case 4:
                if (book.has_order(id)) book.reduce(id, size);
                else                    book.reduce_at(side, price, size);
                break;
            case 3:
                if (book.has_order(id)) book.cancel(id);
                else                    book.reduce_at(side, price, size);
                break;
            default: break;  // 5 hidden (no book change), 6 cross, 7 halt
        }
        ++applied;

        if (!parse_ints(oline, of) || static_cast<int>(of.size()) < 4 * seed_levels)
            continue;
        for (int k = 0; k < levels; ++k) {
            oracle_ask[k] = {of[4 * k + 0], of[4 * k + 1]};
            oracle_bid[k] = {of[4 * k + 2], of[4 * k + 3]};
        }
        const auto eng_ask = book.top_levels(Side::Sell, levels);
        const auto eng_bid = book.top_levels(Side::Buy, levels);

        ++rows;
        const int depth =
            leading_match_depth(eng_ask, oracle_ask, eng_bid, oracle_bid, levels);
        if (depth == 0 && first_l1_break == 0)
            first_l1_break = rows;  // first snapshot whose top of book is wrong
        for (int d = 0; d < depth; ++d)
            ++match_at_depth[d];
        if (depth == levels) {
            ++full_match;
        } else if (first_mismatches.size() < 8) {
            // The first rank that broke -- report which side and the values.
            const bool ask_bad = !rank_matches(eng_ask, oracle_ask, depth);
            const auto& eng    = ask_bad ? eng_ask : eng_bid;
            const auto& ora    = ask_bad ? oracle_ask : oracle_bid;
            const auto [op, os] = ora[depth];
            std::string es = static_cast<int>(eng.size()) > depth
                                 ? std::format("{}x{}", eng[depth].price, eng[depth].qty)
                                 : std::string("empty");
            first_mismatches.push_back(
                {rows, std::format("{} level {}: oracle {}x{} vs engine {}", ask_bad ? "ask" : "bid",
                                   depth + 1, op, os, es)});
        }
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();

    std::cout << std::format("validated {} snapshots ({} applied events)\n", rows, applied);
    if (secs > 0)
        std::cout << std::format("  throughput: {:.2f}M events/sec\n",
                                 static_cast<double>(applied) / secs / 1e6);

    // Depth profile: the headline result. Level 1 (top of book) is exactly
    // reconstructable; agreement falls off with depth as sub-level-N events go
    // unreported by a level-N feed.
    const double denom = rows > 0 ? static_cast<double>(rows) : 1.0;
    std::cout << "  agreement by depth (cumulative, all levels 1..d match):\n";
    for (int d = 0; d < levels; ++d)
        std::cout << std::format("    levels 1..{:<2} : {:9.5f}%  ({} / {})\n", d + 1,
                                 100.0 * static_cast<double>(match_at_depth[d]) / denom,
                                 match_at_depth[d], rows);
    std::cout << std::format("  full {}-level snapshots matched: {} ({:.5f}%)\n", levels, full_match,
                             100.0 * static_cast<double>(full_match) / denom);

    // On real level-N data, deep liquidity that rotates beyond level N is then
    // modified by events the feed omits, so reconstruction is exact only until
    // that loss reaches the top. Report how long top of book held exactly.
    const std::uint64_t top_exact_prefix = first_l1_break ? first_l1_break - 1 : rows;
    std::cout << std::format("  top of book exact for first {} consecutive snapshots\n",
                             top_exact_prefix);

    if (!first_mismatches.empty()) {
        std::cout << "  first divergences (deepest matching level then the break):\n";
        for (const auto& m : first_mismatches)
            std::cout << std::format("    snapshot {}: {}\n", m.row, m.detail);
    }

    // Strict pass: every snapshot reconstructed exactly at all levels. This
    // holds for self-contained feeds (no sub-depth activity), e.g. the
    // regression fixture; full-day level-N exchange data will not reach it.
    return (rows > 0 && full_match == rows) ? 0 : 1;
}
