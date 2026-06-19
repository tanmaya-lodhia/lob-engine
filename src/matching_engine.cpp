#include "lob/matching_engine.hpp"

#include <algorithm>

namespace lob {

Quantity MatchingEngine::cross(OrderId taker, Side side, Price limit, Quantity qty, bool is_market) {
    Quantity   rem = qty;
    const Side opp = opposite(side);
    while (rem > 0) {
        Price      best;
        const bool have = side == Side::Buy ? book_.best_ask(best) : book_.best_bid(best);
        if (!have)
            break;  // opposite side empty
        if (!is_market) {
            // Stop once the best opposite price is no longer marketable.
            if (side == Side::Buy && best > limit)
                break;
            if (side == Side::Sell && best < limit)
                break;
        }
        OrderId  maker;
        Quantity maker_qty;
        if (!book_.front_at(opp, best, maker, maker_qty))
            break;  // invariant: a best price must have a front order
        const Quantity f = std::min(rem, maker_qty);
        fills_.push_back(Fill{taker, maker, side, best, f});
        if (f >= maker_qty)
            book_.cancel(maker);       // maker fully consumed
        else
            book_.reduce(maker, f);    // maker partially filled, keeps its place
        rem -= f;
    }
    return rem;
}

const std::vector<Fill>& MatchingEngine::submit_limit(OrderId id, Side side, Price price,
                                                      Quantity qty, TimeInForce tif) {
    fills_.clear();
    if (qty <= 0)
        return fills_;
    if (tif == TimeInForce::FOK) {
        // All-or-nothing: only proceed if enough marketable liquidity exists.
        if (book_.volume_within(opposite(side), price) < qty)
            return fills_;
    }
    const Quantity rem = cross(id, side, price, qty, /*is_market=*/false);
    if (tif == TimeInForce::GTC && rem > 0)
        book_.add_limit(id, side, price, rem);  // residual rests
    // IOC / FOK: residual discarded.
    return fills_;
}

const std::vector<Fill>& MatchingEngine::submit_market(OrderId id, Side side, Quantity qty) {
    fills_.clear();
    if (qty <= 0)
        return fills_;
    cross(id, side, /*limit=*/0, qty, /*is_market=*/true);
    return fills_;  // market orders never rest
}

bool MatchingEngine::cancel(OrderId id) {
    if (!book_.has_order(id))
        return false;
    book_.cancel(id);
    return true;
}

}  // namespace lob
