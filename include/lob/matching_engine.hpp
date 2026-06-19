#pragma once

#include <vector>

#include "lob/order_book.hpp"
#include "lob/types.hpp"

namespace lob {

// Time in force for an incoming limit order.
//   GTC  good-till-cancel: residual after matching rests in the book.
//   IOC  immediate-or-cancel: match what you can now, discard the residual.
//   FOK  fill-or-kill: only execute if the whole quantity can fill now, else
//        nothing happens at all.
enum class TimeInForce { GTC, IOC, FOK };

// One execution. By price-time priority the trade happens at the resting
// (maker) order's price; the taker is the incoming aggressor.
struct Fill {
    OrderId  taker;
    OrderId  maker;
    Side     taker_side;
    Price    price;  // maker's resting price
    Quantity qty;
};

// A price-time-priority matching engine. Unlike OrderBook (which *replays*
// already-matched events), this engine *decides* the matches: an incoming
// marketable order crosses the book, consuming resting liquidity best-price
// first and, within a price, oldest-order first. It composes OrderBook for
// storage so it inherits that book's tested add / reduce / cancel machinery and
// only adds the crossing logic.
class MatchingEngine {
public:
    // Submit a limit order. It crosses against the opposite side as far as it is
    // marketable; the residual is handled per `tif`. Returns the fills produced
    // (a reference into an internal buffer, valid until the next submit/cancel).
    const std::vector<Fill>& submit_limit(OrderId id, Side side, Price price, Quantity qty,
                                          TimeInForce tif = TimeInForce::GTC);

    // Submit a market order: cross until filled or the book is exhausted; any
    // residual is discarded (market orders never rest).
    const std::vector<Fill>& submit_market(OrderId id, Side side, Quantity qty);

    // Cancel a resting order; false if it was not resting.
    bool cancel(OrderId id);

    // Read-through views of the resting book.
    bool        best_bid(Price& p) const { return book_.best_bid(p); }
    bool        best_ask(Price& p) const { return book_.best_ask(p); }
    Quantity    qty_at(Side s, Price p) const { return book_.qty_at(s, p); }
    bool        has_order(OrderId id) const { return book_.has_order(id); }
    std::size_t resting_order_count() const { return book_.resting_order_count(); }
    std::vector<OrderBook::PriceLevel> top_levels(Side s, int n) const {
        return book_.top_levels(s, n);
    }

private:
    // Cross `qty` of an order on `side` against the book, appending fills.
    // Returns the quantity left unfilled. A market order ignores `limit`.
    Quantity cross(OrderId taker, Side side, Price limit, Quantity qty, bool is_market);

    OrderBook         book_;
    std::vector<Fill> fills_;  // reused per submission
};

}  // namespace lob
