#pragma once

#include <cmath>
#include <cstddef>
#include <vector>

// Pure, allocation-light microstructure estimators that operate on sequences
// already extracted from a reconstructed book (trade signs and mid-prices in
// trade time). Kept free of any I/O or book dependency so they can be unit
// tested against hand-computed answers; the driver in apps/microstructure.cpp
// feeds them.
//
// Conventions follow Bouchaud, Bonart, Donier, Gould, "Trades, Quotes and
// Prices" (2018). Trade sign eps = +1 for a buyer-initiated trade (a market
// order lifting the ask), -1 for seller-initiated.

namespace lob::micro {

// Response function R(l) = < eps_n * (m_{n+l} - m_n) >, the average signed
// mid-price move l trades after a trade. Rising-then-saturating R(l) is the
// signature of market impact. Returned for lags 1..max_lag (index l-1), in the
// price units of `mids`.
inline std::vector<double> response_function(const std::vector<int>& signs,
                                             const std::vector<double>& mids, int max_lag) {
    std::vector<double> r(static_cast<std::size_t>(std::max(0, max_lag)), 0.0);
    const std::size_t   n = signs.size();
    for (int l = 1; l <= max_lag; ++l) {
        if (static_cast<std::size_t>(l) >= n)
            break;
        double      acc = 0.0;
        std::size_t cnt = 0;
        for (std::size_t i = 0; i + static_cast<std::size_t>(l) < n; ++i) {
            acc += signs[i] * (mids[i + static_cast<std::size_t>(l)] - mids[i]);
            ++cnt;
        }
        if (cnt)
            r[static_cast<std::size_t>(l - 1)] = acc / static_cast<double>(cnt);
    }
    return r;
}

// Trade-sign autocorrelation C(l) = < eps_n * eps_{n+l} >. Order flow is
// famously long-ranged: C(l) decays as a slow power law rather than to zero.
// Signs are +-1 so the series is already normalised (C(0) = 1).
inline std::vector<double> sign_autocorrelation(const std::vector<int>& signs, int max_lag) {
    std::vector<double> c(static_cast<std::size_t>(std::max(0, max_lag)), 0.0);
    const std::size_t   n = signs.size();
    for (int l = 1; l <= max_lag; ++l) {
        if (static_cast<std::size_t>(l) >= n)
            break;
        double      acc = 0.0;
        std::size_t cnt = 0;
        for (std::size_t i = 0; i + static_cast<std::size_t>(l) < n; ++i) {
            acc += static_cast<double>(signs[i] * signs[i + static_cast<std::size_t>(l)]);
            ++cnt;
        }
        if (cnt)
            c[static_cast<std::size_t>(l - 1)] = acc / static_cast<double>(cnt);
    }
    return c;
}

// Sample mean and (population) standard deviation of a series.
struct MeanStd {
    double mean = 0.0;
    double std  = 0.0;
};
inline MeanStd mean_std(const std::vector<double>& x) {
    if (x.empty())
        return {};
    double s = 0.0;
    for (double v : x)
        s += v;
    const double m = s / static_cast<double>(x.size());
    double       v = 0.0;
    for (double e : x)
        v += (e - m) * (e - m);
    return {m, std::sqrt(v / static_cast<double>(x.size()))};
}

}  // namespace lob::micro
