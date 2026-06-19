#include "lob/lobster_tape.hpp"

#include <charconv>
#include <cstdlib>
#include <fstream>
#include <string_view>

#include "lob/order_book.hpp"

namespace lob {

namespace {

struct Msg {
    double   time;
    int      type;
    OrderId  id;
    Quantity size;
    Price    price;
    int      dir;
};

bool parse_msg(const std::string& line, Msg& m) {
    std::string_view sv{line};
    std::size_t      pos = 0;
    auto             field = [&]() -> std::string_view {
        std::size_t c = sv.find(',', pos);
        std::string_view tok =
            sv.substr(pos, c == std::string_view::npos ? std::string_view::npos : c - pos);
        pos = c == std::string_view::npos ? sv.size() + 1 : c + 1;
        return tok;
    };
    std::string_view t0 = field();
    m.time              = std::strtod(std::string(t0).c_str(), nullptr);
    long long f[5];
    for (int i = 0; i < 5; ++i) {
        if (pos > sv.size())
            return false;
        std::string_view tok = field();
        if (std::from_chars(tok.data(), tok.data() + tok.size(), f[i]).ec != std::errc{})
            return false;
    }
    m.type  = static_cast<int>(f[0]);
    m.id    = static_cast<OrderId>(f[1]);
    m.size  = static_cast<Quantity>(f[2]);
    m.price = static_cast<Price>(f[3]);
    m.dir   = static_cast<int>(f[4]);
    return true;
}

bool parse_ob_row(const std::string& line, std::vector<long long>& out) {
    out.clear();
    std::string_view sv{line};
    std::size_t      start = 0;
    while (start <= sv.size()) {
        std::size_t c = sv.find(',', start);
        std::string_view tok =
            sv.substr(start, c == std::string_view::npos ? std::string_view::npos : c - start);
        long long v;
        if (std::from_chars(tok.data(), tok.data() + tok.size(), v).ec != std::errc{})
            return false;
        out.push_back(v);
        if (c == std::string_view::npos)
            break;
        start = c + 1;
    }
    return true;
}

bool mid_of(const OrderBook& b, double& mid) {
    Price bid, ask;
    if (!b.best_bid(bid) || !b.best_ask(ask))
        return false;
    mid = 0.5 * (static_cast<double>(bid) + static_cast<double>(ask));
    return true;
}

}  // namespace

bool load_tape(const std::string& message_file, const std::string& orderbook_file, int seed_levels,
               Tape& out, std::string& err) {
    if (seed_levels <= 0) {
        err = "seed_levels must be positive";
        return false;
    }
    std::ifstream msg{message_file}, ob{orderbook_file};
    if (!msg || !ob) {
        err = "cannot open input files";
        return false;
    }

    OrderBook              book;
    std::string            mline, oline;
    std::vector<long long> of;

    if (!std::getline(msg, mline) || !std::getline(ob, oline) || !parse_ob_row(oline, of) ||
        static_cast<int>(of.size()) < 4 * seed_levels) {
        err = "bad or empty input";
        return false;
    }
    OrderId synth = -1;
    for (int k = 0; k < seed_levels; ++k) {
        const long long ap = of[4 * k], as = of[4 * k + 1], bp = of[4 * k + 2], bs = of[4 * k + 3];
        if (as > 0)
            book.add_limit(synth--, Side::Sell, ap, as);
        if (bs > 0)
            book.add_limit(synth--, Side::Buy, bp, bs);
    }

    out = Tape{};
    bool   first_mid = true;
    double lo_mid = 0, hi_mid = 0, first_mid_val = 0;

    // In-progress child order (execution burst).
    bool     active = false;
    double   b_time = 0, b_pre = 0;
    int      b_sign = 0;
    Quantity b_qty  = 0;

    auto finalize = [&](double post_mid) {
        if (active && b_pre > 0 && post_mid > 0 && b_qty > 0)
            out.trades.push_back(Trade{b_time, b_sign, b_qty, b_pre, post_mid});
        active = false;
    };

    while (std::getline(msg, mline)) {
        if (mline.empty())
            continue;
        Msg m;
        if (!parse_msg(mline, m))
            continue;

        double     mid_now;
        const bool have_mid = mid_of(book, mid_now);
        if (have_mid) {
            if (first_mid) {
                lo_mid = hi_mid = first_mid_val = mid_now;
                first_mid                       = false;
            } else {
                lo_mid = std::min(lo_mid, mid_now);
                hi_mid = std::max(hi_mid, mid_now);
            }
        }

        const bool is_exec = m.type == 4 || m.type == 5;
        const int  sign    = m.dir == 1 ? -1 : +1;  // eps = -Direction
        if (is_exec) {
            if (active && m.time == b_time && sign == b_sign) {
                b_qty += m.size;
            } else {
                if (active && have_mid)
                    finalize(mid_now);  // mid after the previous burst
                active = true;
                b_time = m.time;
                b_sign = sign;
                b_qty  = m.size;
                b_pre  = have_mid ? mid_now : 0.0;
            }
            out.total_volume += m.size;
        } else if (active && have_mid) {
            finalize(mid_now);
        }

        const Side side = m.dir == 1 ? Side::Buy : Side::Sell;
        switch (m.type) {
            case 1: book.add_limit(m.id, side, m.price, m.size); break;
            case 2:
            case 4:
                if (book.has_order(m.id)) book.reduce(m.id, m.size);
                else                      book.reduce_at(side, m.price, m.size);
                break;
            case 3:
                if (book.has_order(m.id)) book.cancel(m.id);
                else                      book.reduce_at(side, m.price, m.size);
                break;
            default: break;
        }
    }
    double last_mid;
    if (active && mid_of(book, last_mid))
        finalize(last_mid);

    out.sigma_d = first_mid_val > 0 ? (hi_mid - lo_mid) / first_mid_val : 0.0;
    return !out.trades.empty();
}

}  // namespace lob
