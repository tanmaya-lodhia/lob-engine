#pragma once

#include <cstddef>
#include <map>
#include <unordered_map>
#include <vector>

#include "lob/types.hpp"

namespace lob {

// A single resting order. Orders at one price form a FIFO (time-priority)
// queue, threaded as an intrusive doubly linked list so that an arbitrary
// order can be unlinked in O(1) on cancellation -- we never search the queue.
struct Order {
    OrderId  id;
    Side     side;
    Price    price;
    Quantity qty;  // remaining visible size

    Order* prev = nullptr;  // older order at this price (toward the front)
    Order* next = nullptr;  // newer order at this price (toward the back)
};

// Aggregate state of one price level: the FIFO queue plus running totals so
// that depth queries are O(1) and don't walk the list.
struct Level {
    Price       price       = 0;
    Quantity    total_qty   = 0;
    std::size_t order_count = 0;
    Order*      head        = nullptr;  // front of queue (oldest, executes first)
    Order*      tail        = nullptr;  // back of queue (newest)
};

// Reconstructs a full-depth limit order book from a stream of already-matched
// market events (the LOBSTER / exchange convention). This is a *reconstruction*
// engine, not a matching engine: incoming events describe what the exchange
// already did, so we never cross the book ourselves. A matching mode is a
// planned, separately tested module -- see notes/design.md.
//
// Design baseline: each side is a std::map keyed by price. This is the clear,
// obviously-correct version and the reference the optimized (flat-array) book
// is benchmarked against. The optimization lives behind this same interface.
class OrderBook {
public:
    // Event application -------------------------------------------------------

    // LOBSTER type 1: a new visible limit order joins the back of its queue.
    void add_limit(OrderId id, Side side, Price price, Quantity qty);

    // LOBSTER type 2 (partial cancel) and type 4 (visible execution) both
    // shrink a resting order by `delta`. If the order reaches zero it is
    // removed and its (now empty) level is dropped.
    void reduce(OrderId id, Quantity delta);

    // LOBSTER type 3: the resting order is deleted in full.
    void cancel(OrderId id);

    // Reduce aggregate quantity at a price level without knowing the specific
    // order id, consuming from the front of the FIFO (oldest first). This is
    // the primitive needed to reconcile a *partial-history* feed: events that
    // touch liquidity resting before our window began carry a price and size
    // but reference an order id we never saw. It is also how a market-by-price
    // (aggregated) feed would be applied. Over-reduction is clamped to removal.
    void reduce_at(Side side, Price price, Quantity qty);

    // True if an order with this id is currently resting. Lets a driver choose
    // the precise by-id path for in-window orders and the price-level fallback
    // for pre-existing ones.
    bool has_order(OrderId id) const { return orders_.contains(id); }

    // Queries -----------------------------------------------------------------

    // Best (most aggressive) price on a side; returns false if that side is
    // empty, in which case `out` is left as kNoPrice.
    bool best_bid(Price& out) const;
    bool best_ask(Price& out) const;

    Quantity qty_at(Side side, Price price) const;

    std::size_t resting_order_count() const { return orders_.size(); }

    struct PriceLevel {
        Price    price;
        Quantity qty;
    };

    // Top `n` levels of a side, most aggressive first. Used both for display
    // and for the LOBSTER oracle comparison (validate reconstruction against
    // LOBSTER's own published book snapshots).
    std::vector<PriceLevel> top_levels(Side side, int n) const;

    // Matching support (used by MatchingEngine, which composes this book) ------

    // The oldest resting order at a price level -- the FIFO front, i.e. the one
    // with highest time priority that a marketable order fills first. Returns
    // false if no level exists at that price.
    bool front_at(Side side, Price price, OrderId& id, Quantity& qty) const;

    // Total resting volume on `side` at prices at least as aggressive as
    // `worst` (asks with price <= worst; bids with price >= worst). Used to
    // pre-check a fill-or-kill order before any matching.
    Quantity volume_within(Side side, Price worst) const;

private:
    // std::map keeps prices sorted: asks ascending (best = begin), bids
    // ascending too (best = highest = rbegin). Element addresses in
    // unordered_map are stable across rehash, so the intrusive Order* pointers
    // held by levels remain valid as orders_ grows.
    std::map<Price, Level>               asks_;
    std::map<Price, Level>               bids_;
    std::unordered_map<OrderId, Order>   orders_;

    std::map<Price, Level>&       side_map(Side s)       { return s == Side::Buy ? bids_ : asks_; }
    const std::map<Price, Level>& side_map(Side s) const { return s == Side::Buy ? bids_ : asks_; }

    void link_back(Level& lvl, Order* o);
    void unlink(Level& lvl, Order* o);
    void remove_order(Order* o);  // unlink, drop empty level, erase from orders_
};

}  // namespace lob
