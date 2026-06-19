// Microstructure analytics off the reconstructed book. Seeds from LOBSTER's
// first snapshot (deep, for an accurate inside quote), replays the message file,
// and measures the canonical observables of empirical market microstructure:
//
//   * time-weighted quoted spread and top-of-book depth
//   * per-trade effective spread
//   * trade-sign autocorrelation C(l)        (long-memory of order flow)
//   * the response function R(l)              (market impact in trade time)
//   * single-trade impact binned by trade size
//
// The point: these are measured on a book reconstructed from a real exchange
// feed, not on a model. R(l) and order-flow autocorrelation are the building
// blocks of a propagator study. Note that *single-trade* impact is empirically
// only weakly dependent on size (Bouchaud et al.); the square-root law is a
// metaorder-level effect and needs parent-order labelling -- a natural next
// step that bridges to the metaorder construction used in impact studies.
//
// Trade convention (Bouchaud et al. 2018): a visible (type 4) or hidden (type 5)
// execution of a sell limit order is a buyer-initiated trade, sign +1; execution
// of a buy limit order is seller-initiated, sign -1. So eps = -Direction.
//
// Methodological choices, flagged for honesty:
//   * Consecutive executions sharing a timestamp and sign are aggregated into
//     one trade (a marketable order sweeping several resting orders). This is
//     the standard treatment when the feed gives no aggressor id.
//   * The pre-trade mid is the mid immediately before the trade's first
//     execution; trades are placed in trade time (lag l counts trades).
//
// Usage: microstructure <message_file> <orderbook_file> <seed_levels>
//                       [max_lag=100] [out_dir=.]

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "lob/microstructure.hpp"
#include "lob/order_book.hpp"

namespace {

using namespace lob;

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
    auto             next = [&](std::size_t& start) -> std::string_view {
        std::size_t c = sv.find(',', start);
        std::string_view tok =
            sv.substr(start, c == std::string_view::npos ? std::string_view::npos : c - start);
        start = c == std::string_view::npos ? sv.size() + 1 : c + 1;
        return tok;
    };
    std::size_t      pos = 0;
    std::string_view t0  = next(pos);
    m.time               = std::strtod(std::string(t0).c_str(), nullptr);
    long long f[5];
    for (int i = 0; i < 5; ++i) {
        if (pos > sv.size())
            return false;
        std::string_view tok = next(pos);
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

void write_csv(const std::string& path, const std::string& header,
               const std::vector<std::string>& rows) {
    std::ofstream f{path};
    if (!f) {
        std::cerr << std::format("warning: could not write {}\n", path);
        return;
    }
    f << header << '\n';
    for (const auto& r : rows)
        f << r << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4 || argc > 6) {
        std::cerr << "usage: microstructure <message_file> <orderbook_file> <seed_levels> "
                     "[max_lag=100] [out_dir=.]\n";
        return 2;
    }
    const int   seed_levels = std::atoi(argv[3]);
    const int   max_lag     = argc >= 5 ? std::atoi(argv[4]) : 100;
    std::string out_dir     = argc >= 6 ? argv[5] : ".";
    if (seed_levels <= 0 || max_lag <= 0) {
        std::cerr << "seed_levels and max_lag must be positive\n";
        return 2;
    }

    std::ifstream msg{argv[1]}, ob{argv[2]};
    if (!msg || !ob) {
        std::cerr << "error: cannot open input files\n";
        return 1;
    }

    OrderBook              book;
    std::string            mline, oline;
    std::vector<long long> of;

    // Seed from the first orderbook snapshot, then discard the first message.
    if (!std::getline(msg, mline) || !std::getline(ob, oline) || !parse_ob_row(oline, of) ||
        static_cast<int>(of.size()) < 4 * seed_levels) {
        std::cerr << "error: bad or empty input\n";
        return 1;
    }
    OrderId synth = -1;
    for (int k = 0; k < seed_levels; ++k) {
        const long long ap = of[4 * k], as = of[4 * k + 1], bp = of[4 * k + 2], bs = of[4 * k + 3];
        if (as > 0)
            book.add_limit(synth--, Side::Sell, ap, as);
        if (bs > 0)
            book.add_limit(synth--, Side::Buy, bp, bs);
    }

    // Per-trade series (trade time).
    std::vector<int>      signs;
    std::vector<double>   mids;       // pre-trade mid
    std::vector<Quantity> tsizes;     // aggregated trade size
    std::vector<double>   eff_bps;    // per-trade effective spread, bps

    // In-progress trade aggregation.
    bool     t_active = false;
    double   t_time = 0, t_premid = 0, t_notional = 0;
    int      t_sign = 0;
    Quantity t_qty = 0;
    auto flush = [&]() {
        if (!t_active)
            return;
        if (t_premid > 0 && t_qty > 0) {
            signs.push_back(t_sign);
            mids.push_back(t_premid);
            tsizes.push_back(t_qty);
            const double vwap = t_notional / static_cast<double>(t_qty);
            eff_bps.push_back(2.0 * t_sign * (vwap - t_premid) / t_premid * 1e4);
        }
        t_active = false;
    };

    // Time-weighted book stats.
    double tw_time = 0, tw_spread = 0, tw_spread_bps = 0, tw_depth = 0;
    double prev_t = 0, cur_spread = 0, cur_spread_bps = 0, cur_depth = 0;
    bool   have_prev = false, cur_valid = false;
    std::uint64_t applied = 0, n_exec = 0;
    Quantity      traded_volume = 0;

    auto refresh_book_stats = [&]() {
        Price bid, ask;
        if (book.best_bid(bid) && book.best_ask(ask)) {
            const double mid = 0.5 * (static_cast<double>(bid) + static_cast<double>(ask));
            cur_spread       = static_cast<double>(ask - bid);
            cur_spread_bps   = cur_spread / mid * 1e4;
            cur_depth = static_cast<double>(book.qty_at(Side::Buy, bid) + book.qty_at(Side::Sell, ask));
            cur_valid = true;
        } else {
            cur_valid = false;
        }
    };
    refresh_book_stats();

    while (std::getline(msg, mline)) {
        if (mline.empty())
            continue;
        Msg m;
        if (!parse_msg(mline, m))
            continue;

        // Weight the interval since the previous event by the state that held.
        if (have_prev && cur_valid) {
            const double dt = m.time - prev_t;
            if (dt > 0) {
                tw_time += dt;
                tw_spread += cur_spread * dt;
                tw_spread_bps += cur_spread_bps * dt;
                tw_depth += cur_depth * dt;
            }
        }

        const bool is_exec = m.type == 4 || m.type == 5;
        const int  sign    = m.dir == 1 ? -1 : +1;  // eps = -Direction
        double     mid_now = 0;
        const bool have_mid = mid_of(book, mid_now);

        if (is_exec) {
            if (t_active && m.time == t_time && sign == t_sign) {
                t_qty += m.size;
                t_notional += static_cast<double>(m.size) * static_cast<double>(m.price);
            } else {
                flush();
                t_active   = true;
                t_time     = m.time;
                t_sign     = sign;
                t_qty      = m.size;
                t_notional = static_cast<double>(m.size) * static_cast<double>(m.price);
                t_premid   = have_mid ? mid_now : 0.0;
            }
            ++n_exec;
            traded_volume += m.size;
        } else if (t_active) {
            flush();
        }

        // Apply the event (by-id when known, price-level fallback otherwise).
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
            default: break;  // 5 hidden: no visible-book change
        }
        ++applied;
        refresh_book_stats();
        prev_t    = m.time;
        have_prev = true;
    }
    flush();

    const std::size_t N = signs.size();
    if (N < 2) {
        std::cerr << "error: too few trades reconstructed\n";
        return 1;
    }

    // Estimators.
    const auto R = micro::response_function(signs, mids, max_lag);
    const auto C = micro::sign_autocorrelation(signs, max_lag);

    // Per-trade mid log-returns (volatility) and a reference mid for bps.
    std::vector<double> logret;
    logret.reserve(N - 1);
    double mid_sum = 0;
    for (std::size_t i = 0; i < N; ++i)
        mid_sum += mids[i];
    const double mid_ref = mid_sum / static_cast<double>(N);
    for (std::size_t i = 1; i < N; ++i)
        logret.push_back(std::log(mids[i] / mids[i - 1]));
    const auto vol = micro::mean_std(logret);
    const auto esp = micro::mean_std(eff_bps);

    // Aggregate impact by trade size: immediate signed mid move sign_n*(m_{n+1}-m_n),
    // grouped into log-spaced size bins.
    constexpr int               kBins = 8;
    std::vector<std::uint64_t>  bin_n(kBins, 0);
    std::vector<double>         bin_size(kBins, 0), bin_imp(kBins, 0);
    Quantity smin = tsizes[0], smax = tsizes[0];
    for (Quantity q : tsizes) {
        smin = std::min(smin, q);
        smax = std::max(smax, q);
    }
    const double lmin = std::log(static_cast<double>(std::max<Quantity>(1, smin)));
    const double lmax = std::log(static_cast<double>(std::max<Quantity>(2, smax)));
    const double lspan = std::max(1e-9, lmax - lmin);
    for (std::size_t i = 0; i + 1 < N; ++i) {
        const double frac = (std::log(static_cast<double>(std::max<Quantity>(1, tsizes[i]))) - lmin) / lspan;
        int          b    = static_cast<int>(frac * kBins);
        b                 = std::clamp(b, 0, kBins - 1);
        bin_n[b]++;
        bin_size[b] += static_cast<double>(tsizes[i]);
        bin_imp[b] += signs[i] * (mids[i + 1] - mids[i]);
    }

    // --- report --------------------------------------------------------------
    std::cout << std::format("events applied : {}\n", applied);
    std::cout << std::format("executions     : {}  aggregated into {} trades\n", n_exec, N);
    std::cout << std::format("traded volume  : {}\n", traded_volume);
    if (tw_time > 0) {
        std::cout << std::format("time-weighted spread : {:.3f} ticks  ({:.3f} bps)\n",
                                 tw_spread / tw_time, tw_spread_bps / tw_time);
        std::cout << std::format("time-weighted top depth: {:.1f} (bid+ask, shares)\n",
                                 tw_depth / tw_time);
    }
    std::cout << std::format("mean effective spread : {:.3f} bps\n", esp.mean);
    std::cout << std::format("per-trade mid vol     : {:.3f} bps (1 sigma log-return)\n",
                             vol.std * 1e4);
    std::cout << std::format("response R(1)={:.4f}  R(10)={:.4f}  R(50)={:.4f} ticks\n", R[0],
                             max_lag >= 10 ? R[9] : 0.0, max_lag >= 50 ? R[49] : 0.0);
    std::cout << std::format("sign autocorr C(1)={:.4f}  C(10)={:.4f}  C(50)={:.4f}\n", C[0],
                             max_lag >= 10 ? C[9] : 0.0, max_lag >= 50 ? C[49] : 0.0);

    // --- csv -----------------------------------------------------------------
    std::vector<std::string> rows;
    for (int l = 1; l <= max_lag; ++l)
        rows.push_back(std::format("{},{:.8f},{:.8f}", l, R[static_cast<std::size_t>(l - 1)],
                                   R[static_cast<std::size_t>(l - 1)] / mid_ref * 1e4));
    write_csv(out_dir + "/response_function.csv", "lag,R_ticks,R_bps", rows);

    rows.clear();
    for (int l = 1; l <= max_lag; ++l)
        rows.push_back(std::format("{},{:.8f}", l, C[static_cast<std::size_t>(l - 1)]));
    write_csv(out_dir + "/sign_autocorrelation.csv", "lag,C", rows);

    rows.clear();
    for (int b = 0; b < kBins; ++b) {
        if (bin_n[b] == 0)
            continue;
        const double ms = bin_size[b] / static_cast<double>(bin_n[b]);
        const double mi = bin_imp[b] / static_cast<double>(bin_n[b]);
        rows.push_back(std::format("{},{},{:.2f},{:.6f},{:.6f}", b, bin_n[b], ms, mi,
                                   mi / mid_ref * 1e4));
    }
    write_csv(out_dir + "/impact_by_size.csv", "bin,n_trades,mean_size,mean_impact_ticks,mean_impact_bps",
              rows);

    std::cout << std::format("wrote response_function.csv, sign_autocorrelation.csv, "
                             "impact_by_size.csv to {}\n",
                             out_dir);
    return 0;
}
