#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

#include "lob/types.hpp"

namespace lob {

// A cache-friendly limit order book that replaces OrderBook's std::map price
// levels with a flat array indexed by tick offset, and its per-order map nodes
// with a pooled, index-linked free list. Same semantics as OrderBook (verified
// against it event-for-event in the benchmark), traded for O(1) price access
// and far less pointer chasing / allocation on the hot path.
//
// The flat array spans a fixed price band [base, base + span) in raw price
// ticks. This suits replay/backtest, where the instrument's range over the
// window is known up front; a live engine would recenter the band instead.
// Prices outside the band are counted (out_of_band) and ignored rather than
// crashing, so a mis-sized band degrades visibly instead of silently.
class FlatBook {
public:
    static constexpr std::size_t kNil = std::numeric_limits<std::size_t>::max();

    FlatBook(Price base, std::size_t span);

    void add_limit(OrderId id, Side side, Price price, Quantity qty);
    void reduce(OrderId id, Quantity delta);
    void cancel(OrderId id);
    void reduce_at(Side side, Price price, Quantity qty);

    bool has_order(OrderId id) const { return index_.contains(id); }

    bool     best_bid(Price& out) const;
    bool     best_ask(Price& out) const;
    Quantity qty_at(Side side, Price price) const;
    std::size_t resting_order_count() const { return index_.size(); }

    struct PriceLevel {
        Price    price;
        Quantity qty;
    };
    std::vector<PriceLevel> top_levels(Side side, int n) const;

    // Number of events dropped because their price fell outside the band.
    // Should be zero for a correctly sized book.
    std::uint64_t out_of_band() const { return oob_; }

private:
    struct Node {
        OrderId     id;
        Quantity    qty;
        std::size_t price_idx;
        std::size_t prev;
        std::size_t next;
        Side        side;
    };
    struct Level {
        Quantity      total_qty   = 0;
        std::uint32_t order_count = 0;
        std::size_t   head        = kNil;
        std::size_t   tail        = kNil;
    };

    // Returns kNil if the price is outside the band.
    std::size_t idx_of(Price price) const;

    std::vector<Level>&       side_levels(Side s) { return s == Side::Buy ? bid_levels_ : ask_levels_; }
    const std::vector<Level>& side_levels(Side s) const {
        return s == Side::Buy ? bid_levels_ : ask_levels_;
    }

    std::size_t alloc_node();
    void        free_node(std::size_t n);
    void        link_back(Level& lvl, std::size_t n);
    void        unlink(Level& lvl, std::size_t n);
    void        remove_node(std::size_t n);

    void advance_best_ask();  // best ask emptied: scan upward for the next
    void advance_best_bid();  // best bid emptied: scan downward for the next

    // Bids and asks get independent price arrays. Sharing one array per price
    // breaks the moment the book transiently crosses (an ask at or below a bid),
    // because a single level cannot hold both sides at the same price.
    Price                    base_;
    std::size_t              span_;
    std::vector<Level>       ask_levels_;
    std::vector<Level>       bid_levels_;
    std::vector<Node>        pool_;
    std::vector<std::size_t> free_;
    std::unordered_map<OrderId, std::size_t> index_;

    std::size_t   best_bid_idx_ = kNil;  // highest occupied bid level
    std::size_t   best_ask_idx_ = kNil;  // lowest occupied ask level
    std::uint64_t oob_          = 0;
};

}  // namespace lob
