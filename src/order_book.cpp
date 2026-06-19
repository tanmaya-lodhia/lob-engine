#include "lob/order_book.hpp"

#include <cassert>

namespace lob {

void OrderBook::link_back(Level& lvl, Order* o) {
    o->prev = lvl.tail;
    o->next = nullptr;
    if (lvl.tail)
        lvl.tail->next = o;
    else
        lvl.head = o;  // first order at this level
    lvl.tail = o;
    lvl.total_qty += o->qty;
    ++lvl.order_count;
}

void OrderBook::unlink(Level& lvl, Order* o) {
    if (o->prev)
        o->prev->next = o->next;
    else
        lvl.head = o->next;  // was the front
    if (o->next)
        o->next->prev = o->prev;
    else
        lvl.tail = o->prev;  // was the back
    lvl.total_qty -= o->qty;
    --lvl.order_count;
}

void OrderBook::remove_order(Order* o) {
    auto& book = side_map(o->side);
    auto  it   = book.find(o->price);
    assert(it != book.end() && "level must exist for a resting order");
    Level& lvl = it->second;
    unlink(lvl, o);
    if (lvl.order_count == 0)
        book.erase(it);
    orders_.erase(o->id);
}

void OrderBook::add_limit(OrderId id, Side side, Price price, Quantity qty) {
    // Duplicate ids would corrupt the intrusive list; the exchange guarantees
    // uniqueness, so an existing id signals a malformed/garbled stream.
    auto [it, inserted] = orders_.try_emplace(id, Order{id, side, price, qty, nullptr, nullptr});
    assert(inserted && "duplicate order id in event stream");
    if (!inserted)
        return;

    Level& lvl = side_map(side)[price];
    lvl.price  = price;
    link_back(lvl, &it->second);
}

void OrderBook::reduce(OrderId id, Quantity delta) {
    auto it = orders_.find(id);
    if (it == orders_.end())
        return;  // reducing an unknown order: tolerate (e.g. hidden/partial-history)
    Order* o = &it->second;

    if (delta >= o->qty) {
        remove_order(o);
        return;
    }
    // Partial: shrink in place. Time priority is preserved (queue position
    // does not change when size is reduced).
    auto& book = side_map(o->side);
    book.at(o->price).total_qty -= delta;
    o->qty -= delta;
}

void OrderBook::cancel(OrderId id) {
    auto it = orders_.find(id);
    if (it == orders_.end())
        return;
    remove_order(&it->second);
}

void OrderBook::reduce_at(Side side, Price price, Quantity qty) {
    auto& book = side_map(side);
    auto  it   = book.find(price);
    if (it == book.end())
        return;  // nothing resting here; tolerate (pre-window state we never saw)
    Level&   lvl       = it->second;
    Quantity remaining = qty;
    while (remaining > 0 && lvl.head != nullptr) {
        Order* o = lvl.head;
        if (o->qty > remaining) {
            o->qty        -= remaining;  // partially fills the front order
            lvl.total_qty -= remaining;
            remaining = 0;
        } else {
            remaining -= o->qty;  // front order fully consumed
            unlink(lvl, o);       // advances head; fixes totals/count
            orders_.erase(o->id);
        }
    }
    if (lvl.order_count == 0)
        book.erase(it);
}

bool OrderBook::best_bid(Price& out) const {
    if (bids_.empty()) {
        out = kNoPrice;
        return false;
    }
    out = bids_.rbegin()->first;  // highest bid
    return true;
}

bool OrderBook::best_ask(Price& out) const {
    if (asks_.empty()) {
        out = kNoPrice;
        return false;
    }
    out = asks_.begin()->first;  // lowest ask
    return true;
}

bool OrderBook::front_at(Side side, Price price, OrderId& id, Quantity& qty) const {
    const auto& book = side_map(side);
    auto        it   = book.find(price);
    if (it == book.end() || it->second.head == nullptr)
        return false;
    const Order* h = it->second.head;
    id             = h->id;
    qty            = h->qty;
    return true;
}

Quantity OrderBook::volume_within(Side side, Price worst) const {
    const auto& book = side_map(side);
    Quantity    sum  = 0;
    if (side == Side::Sell) {
        // asks ascending: take levels with price <= worst
        for (auto it = book.begin(); it != book.end() && it->first <= worst; ++it)
            sum += it->second.total_qty;
    } else {
        // bids descending from the top: take levels with price >= worst
        for (auto it = book.rbegin(); it != book.rend() && it->first >= worst; ++it)
            sum += it->second.total_qty;
    }
    return sum;
}

Quantity OrderBook::qty_at(Side side, Price price) const {
    const auto& book = side_map(side);
    auto        it   = book.find(price);
    return it == book.end() ? 0 : it->second.total_qty;
}

std::vector<OrderBook::PriceLevel> OrderBook::top_levels(Side side, int n) const {
    std::vector<PriceLevel> out;
    if (n <= 0)
        return out;
    out.reserve(static_cast<std::size_t>(n));
    const auto& book = side_map(side);
    if (side == Side::Buy) {
        // Bids: most aggressive = highest price = reverse iteration.
        for (auto it = book.rbegin(); it != book.rend() && static_cast<int>(out.size()) < n; ++it)
            out.push_back({it->first, it->second.total_qty});
    } else {
        for (auto it = book.begin(); it != book.end() && static_cast<int>(out.size()) < n; ++it)
            out.push_back({it->first, it->second.total_qty});
    }
    return out;
}

}  // namespace lob
