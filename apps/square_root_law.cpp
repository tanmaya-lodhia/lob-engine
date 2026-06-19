// The square-root law of market impact, via the Maitrier-Loeper-Bouchaud
// metaorder construction (arXiv:2503.18199) applied to a reconstructed book.
//
// Real metaorder studies need proprietary per-trader data. MLB's insight is
// that the square-root law does not depend on *which* trader sends what, so it
// can be recovered from the anonymous public tape by a random "mapping":
//
//   1. Reconstruct the trade tape from the LOBSTER feed (lobster_tape).
//   2. Assign each trade to one of N synthetic traders at random, preserving
//      chronological order.
//   3. A metaorder = a maximal same-sign run within one trader's sequence
//      (keep only runs with > 1 child order).
//   4. For each metaorder of volume Q: impact I = eps * (ln p_e - ln p_s),
//      p_s = mid before the first child, p_e = mid after the last child.
//   5. Average over many random mappings, bin by Q / V_D, and fit
//        I(Q) / sigma_D = Y * (Q / V_D)^delta.
//      The square-root law is delta ~ 0.5, prefactor Y ~ 0.5..1.
//
// This is the milestone that puts the impact-diffusivity line of work onto a
// genuinely reconstructed book rather than synthetic prices.
//
// Usage: square_root_law <message_file> <orderbook_file> <seed_levels>
//                        [n_traders=20] [realizations=500] [out_dir=.]

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <format>
#include <random>
#include <string>
#include <vector>

#include "lob/lobster_tape.hpp"
#include "lob/microstructure.hpp"

namespace {

constexpr int    kBins = 24;
constexpr double kLo   = 1e-6;  // Q / V_D range (log-spaced)
constexpr double kHi   = 1e0;

// Below this Q/V_D, impact plateaus at the spread/discreteness floor (the
// smallest metaorders all cross ~one spread); MLB exclude this small-Q region.
// The square-root scaling is measured above it, on bins with enough mass.
constexpr double      kScalingFloor   = 3e-4;
constexpr std::uint64_t kScalingMinN  = 2000;

int bin_of(double x) {
    if (x <= 0.0)
        return -1;
    const double f = (std::log(x) - std::log(kLo)) / (std::log(kHi) - std::log(kLo));
    return std::clamp(static_cast<int>(f * kBins), 0, kBins - 1);
}

}  // namespace

int main(int argc, char** argv) {
    using namespace lob;
    if (argc < 4 || argc > 7) {
        std::cerr << "usage: square_root_law <message_file> <orderbook_file> <seed_levels> "
                     "[n_traders=20] [realizations=500] [out_dir=.]\n";
        return 2;
    }
    const int   seed_levels  = std::atoi(argv[3]);
    const int   n_traders    = argc >= 5 ? std::atoi(argv[4]) : 20;
    const int   realizations = argc >= 6 ? std::atoi(argv[5]) : 500;
    std::string out_dir      = argc >= 7 ? argv[6] : ".";
    if (seed_levels <= 0 || n_traders <= 0 || realizations <= 0) {
        std::cerr << "seed_levels, n_traders, realizations must be positive\n";
        return 2;
    }

    Tape        tape;
    std::string err;
    if (!load_tape(argv[1], argv[2], seed_levels, tape, err)) {
        std::cerr << std::format("error: {}\n", err);
        return 1;
    }
    const auto& tr = tape.trades;
    const auto  n  = tr.size();
    if (tape.total_volume <= 0 || tape.sigma_d <= 0.0) {
        std::cerr << "error: degenerate normalizers (V_D or sigma_D)\n";
        return 1;
    }
    const double VD = static_cast<double>(tape.total_volume);

    // Precompute log mids.
    std::vector<double> lpre(n), lpost(n);
    for (std::size_t i = 0; i < n; ++i) {
        lpre[i]  = std::log(tr[i].pre_mid);
        lpost[i] = std::log(tr[i].post_mid);
    }

    std::cout << std::format("tape: {} child orders, V_D={}, sigma_D={:.5f}\n", n, tape.total_volume,
                             tape.sigma_d);
    std::cout << std::format("mapping: {} traders (homogeneous), {} realizations\n", n_traders,
                             realizations);

    std::vector<double>        sum_x(kBins, 0.0), sum_y(kBins, 0.0);
    std::vector<std::uint64_t> cnt(kBins, 0);
    std::uint64_t              total_meta = 0;

    std::mt19937                       rng{20240607u};
    std::uniform_int_distribution<int> pick{0, n_traders - 1};
    std::vector<std::vector<std::uint32_t>> bucket(static_cast<std::size_t>(n_traders));

    for (int rep = 0; rep < realizations; ++rep) {
        for (auto& b : bucket)
            b.clear();
        // Assign each trade to a trader, preserving chronological order within
        // each trader's bucket (we append in time order).
        for (std::uint32_t i = 0; i < n; ++i)
            bucket[static_cast<std::size_t>(pick(rng))].push_back(i);

        for (const auto& b : bucket) {
            std::size_t i = 0;
            while (i < b.size()) {
                const int   s     = tr[b[i]].sign;
                std::size_t j     = i;
                Quantity    q     = 0;
                while (j < b.size() && tr[b[j]].sign == s) {
                    q += tr[b[j]].volume;
                    ++j;
                }
                const std::size_t len = j - i;
                if (len > 1) {  // metaorder: more than one child order
                    const std::uint32_t first = b[i];
                    const std::uint32_t last  = b[j - 1];
                    const double impact = s * (lpost[last] - lpre[first]);  // I = eps*(ln pe - ln ps)
                    const double x      = static_cast<double>(q) / VD;
                    const double y      = impact / tape.sigma_d;
                    const int    bin    = bin_of(x);
                    if (bin >= 0) {
                        sum_x[static_cast<std::size_t>(bin)] += x;
                        sum_y[static_cast<std::size_t>(bin)] += y;
                        ++cnt[static_cast<std::size_t>(bin)];
                        ++total_meta;
                    }
                }
                i = j;
            }
        }
    }

    // Bin means. Two fits: one across all populated bins (dragged down by the
    // small-Q plateau), and one over the scaling regime MLB actually measure.
    std::vector<double>      all_x, all_y, sc_x, sc_y;
    std::vector<std::string> rows;
    for (int b = 0; b < kBins; ++b) {
        const std::size_t c = cnt[static_cast<std::size_t>(b)];
        if (c == 0)
            continue;
        const double mx = sum_x[static_cast<std::size_t>(b)] / static_cast<double>(c);
        const double my = sum_y[static_cast<std::size_t>(b)] / static_cast<double>(c);
        rows.push_back(std::format("{:.8e},{:.8e},{}", mx, my, c));
        if (c >= 50 && my > 0.0) {
            all_x.push_back(mx);
            all_y.push_back(my);
        }
        if (mx >= kScalingFloor && c >= kScalingMinN && my > 0.0) {
            sc_x.push_back(mx);
            sc_y.push_back(my);
        }
    }

    const auto all_fit = micro::fit_power_law(all_x, all_y);
    const auto sc_fit  = micro::fit_power_law(sc_x, sc_y);

    std::cout << std::format("metaorders generated: {}\n", total_meta);
    std::cout << std::format("all bins      ({:2} pts): I/sigma_D = {:.3f} * (Q/V_D)^{:.3f}  "
                             "(R^2={:.3f})\n",
                             all_fit.n, all_fit.prefactor, all_fit.exponent, all_fit.r2);
    std::cout << std::format("scaling regime ({:2} pts, Q/V_D>={:.0e}): "
                             "I/sigma_D = {:.3f} * (Q/V_D)^{:.3f}  (R^2={:.3f})\n",
                             sc_fit.n, kScalingFloor, sc_fit.prefactor, sc_fit.exponent, sc_fit.r2);
    std::cout << "  square-root law predicts exponent ~0.5; small-Q plateau excluded (MLB)\n";

    std::ofstream f{out_dir + "/square_root_law.csv"};
    if (f) {
        f << "Q_over_VD,I_over_sigmaD,n_metaorders\n";
        for (const auto& r : rows)
            f << r << '\n';
        std::cout << std::format("wrote square_root_law.csv to {}\n", out_dir);
    } else {
        std::cerr << "warning: could not write square_root_law.csv\n";
    }
    return 0;
}
