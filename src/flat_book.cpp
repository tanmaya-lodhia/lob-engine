#include "lob/flat_book.hpp"

#include <cassert>

namespace lob {

FlatBook::FlatBook(Price base, std::size_t span)
    : base_(base), span_(span), ask_levels_(span), bid_levels_(span) {}

std::size_t FlatBook::idx_of(Price price) const {
    if (price < base_)
        return kNil;
    const auto i = static_cast<std::size_t>(price - base_);
    return i < span_ ? i : kNil;
}

std::size_t FlatBook::alloc_node() {
    if (!free_.empty()) {
        const std::size_t n = free_.back();
        free_.pop_back();
        return n;
    }
    pool_.push_back(Node{});
    return pool_.size() - 1;
}

void FlatBook::free_node(std::size_t n) { free_.push_back(n); }

void FlatBook::link_back(Level& lvl, std::size_t n) {
    Node& node = pool_[n];
    node.prev  = lvl.tail;
    node.next  = kNil;
    if (lvl.tail != kNil)
        pool_[lvl.tail].next = n;
    else
        lvl.head = n;
    lvl.tail = n;
    lvl.total_qty += node.qty;
    ++lvl.order_count;
}

void FlatBook::unlink(Level& lvl, std::size_t n) {
    Node& node = pool_[n];
    if (node.prev != kNil)
        pool_[node.prev].next = node.next;
    else
        lvl.head = node.next;
    if (node.next != kNil)
        pool_[node.next].prev = node.prev;
    else
        lvl.tail = node.prev;
    lvl.total_qty -= node.qty;
    --lvl.order_count;
}

void FlatBook::remove_node(std::size_t n) {
    Node&      node = pool_[n];
    const auto pidx = node.price_idx;
    const Side side = node.side;
    Level&     lvl  = side_levels(side)[pidx];
    unlink(lvl, n);
    index_.erase(node.id);
    free_node(n);
    if (lvl.order_count == 0) {
        if (side == Side::Sell && pidx == best_ask_idx_)
            advance_best_ask();
        else if (side == Side::Buy && pidx == best_bid_idx_)
            advance_best_bid();
    }
}

void FlatBook::advance_best_ask() {
    for (std::size_t i = best_ask_idx_ + 1; i < span_; ++i)
        if (ask_levels_[i].order_count > 0) {
            best_ask_idx_ = i;
            return;
        }
    best_ask_idx_ = kNil;
}

void FlatBook::advance_best_bid() {
    // best_bid_idx_ > 0 here unless it was the very bottom of the band.
    for (std::size_t i = best_bid_idx_; i-- > 0;)
        if (bid_levels_[i].order_count > 0) {
            best_bid_idx_ = i;
            return;
        }
    best_bid_idx_ = kNil;
}

void FlatBook::add_limit(OrderId id, Side side, Price price, Quantity qty) {
    // A re-used id while one is still resting would corrupt the index; match
    // OrderBook by keeping the original and ignoring the duplicate.
    if (index_.contains(id))
        return;
    const std::size_t idx = idx_of(price);
    if (idx == kNil) {
        ++oob_;
        return;
    }
    const std::size_t n    = alloc_node();
    pool_[n]               = Node{id, qty, idx, kNil, kNil, side};
    index_[id]             = n;
    link_back(side_levels(side)[idx], n);

    if (side == Side::Sell) {
        if (best_ask_idx_ == kNil || idx < best_ask_idx_)
            best_ask_idx_ = idx;
    } else {
        if (best_bid_idx_ == kNil || idx > best_bid_idx_)
            best_bid_idx_ = idx;
    }
}

void FlatBook::reduce(OrderId id, Quantity delta) {
    auto it = index_.find(id);
    if (it == index_.end())
        return;
    const std::size_t n    = it->second;
    Node&             node = pool_[n];
    if (delta >= node.qty) {
        remove_node(n);
        return;
    }
    side_levels(node.side)[node.price_idx].total_qty -= delta;
    node.qty -= delta;
}

void FlatBook::cancel(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end())
        return;
    remove_node(it->second);
}

void FlatBook::reduce_at(Side side, Price price, Quantity qty) {
    const std::size_t idx = idx_of(price);
    if (idx == kNil)
        return;
    Level&   lvl       = side_levels(side)[idx];
    Quantity remaining = qty;
    while (remaining > 0 && lvl.head != kNil) {
        const std::size_t n    = lvl.head;
        Node&             node = pool_[n];
        if (node.qty > remaining) {
            node.qty      -= remaining;
            lvl.total_qty -= remaining;
            remaining = 0;
        } else {
            remaining -= node.qty;
            index_.erase(node.id);
            unlink(lvl, n);
            free_node(n);
        }
    }
    if (lvl.order_count == 0) {
        if (side == Side::Sell && idx == best_ask_idx_)
            advance_best_ask();
        else if (side == Side::Buy && idx == best_bid_idx_)
            advance_best_bid();
    }
}

bool FlatBook::best_bid(Price& out) const {
    if (best_bid_idx_ == kNil) {
        out = kNoPrice;
        return false;
    }
    out = base_ + static_cast<Price>(best_bid_idx_);
    return true;
}

bool FlatBook::best_ask(Price& out) const {
    if (best_ask_idx_ == kNil) {
        out = kNoPrice;
        return false;
    }
    out = base_ + static_cast<Price>(best_ask_idx_);
    return true;
}

Quantity FlatBook::qty_at(Side side, Price price) const {
    const std::size_t idx = idx_of(price);
    return idx == kNil ? 0 : side_levels(side)[idx].total_qty;
}

std::vector<FlatBook::PriceLevel> FlatBook::top_levels(Side side, int n) const {
    std::vector<PriceLevel> out;
    if (n <= 0)
        return out;
    out.reserve(static_cast<std::size_t>(n));
    if (side == Side::Sell) {
        for (std::size_t i = best_ask_idx_;
             i != kNil && i < span_ && static_cast<int>(out.size()) < n; ++i)
            if (ask_levels_[i].order_count > 0)
                out.push_back({base_ + static_cast<Price>(i), ask_levels_[i].total_qty});
    } else {
        for (std::size_t i = best_bid_idx_;
             i != kNil && static_cast<int>(out.size()) < n; --i) {
            if (bid_levels_[i].order_count > 0)
                out.push_back({base_ + static_cast<Price>(i), bid_levels_[i].total_qty});
            if (i == 0)
                break;
        }
    }
    return out;
}

}  // namespace lob
