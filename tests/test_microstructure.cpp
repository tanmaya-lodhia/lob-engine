#include <doctest/doctest.h>

#include "lob/microstructure.hpp"

using namespace lob::micro;
using doctest::Approx;

TEST_CASE("response function matches hand computation") {
    // signs and mids in trade time.
    std::vector<int>    signs = {+1, -1, +1, -1};
    std::vector<double> mids  = {10, 11, 12, 11};
    auto                r     = response_function(signs, mids, 3);
    REQUIRE(r.size() == 3);
    CHECK(r[0] == Approx(-1.0 / 3.0));  // lag 1
    CHECK(r[1] == Approx(1.0));         // lag 2
    CHECK(r[2] == Approx(1.0));         // lag 3
}

TEST_CASE("sign autocorrelation matches hand computation") {
    std::vector<int> signs = {+1, -1, +1, -1};
    auto             c     = sign_autocorrelation(signs, 3);
    REQUIRE(c.size() == 3);
    CHECK(c[0] == Approx(-1.0));  // perfectly anti-correlated at lag 1
    CHECK(c[1] == Approx(1.0));   // back in phase at lag 2
    CHECK(c[2] == Approx(-1.0));
}

TEST_CASE("persistent flow gives positive sign autocorrelation") {
    std::vector<int> signs = {+1, +1, +1, +1, +1};
    auto             c     = sign_autocorrelation(signs, 2);
    CHECK(c[0] == Approx(1.0));
    CHECK(c[1] == Approx(1.0));
}

TEST_CASE("lags beyond the series are zero, not garbage") {
    std::vector<int>    signs = {+1, -1};
    std::vector<double> mids  = {10, 10.5};
    auto                r     = response_function(signs, mids, 5);
    REQUIRE(r.size() == 5);
    CHECK(r[0] == Approx(+0.5));  // lag 1 defined
    for (std::size_t i = 1; i < r.size(); ++i)
        CHECK(r[i] == Approx(0.0));  // lags >= 2 undefined -> zero
}

TEST_CASE("mean_std on a known series") {
    std::vector<double> x = {2, 4, 4, 4, 5, 5, 7, 9};
    auto                m = mean_std(x);
    CHECK(m.mean == Approx(5.0));
    CHECK(m.std == Approx(2.0));  // population standard deviation
}

TEST_CASE("power-law fit recovers a known exponent and prefactor") {
    // y = 3 * x^0.5 exactly -> exponent 0.5, prefactor 3, perfect fit.
    std::vector<double> x = {1, 2, 4, 8, 16, 32};
    std::vector<double> y;
    for (double v : x)
        y.push_back(3.0 * std::sqrt(v));
    auto f = fit_power_law(x, y);
    CHECK(f.n == 6);
    CHECK(f.exponent == Approx(0.5));
    CHECK(f.prefactor == Approx(3.0));
    CHECK(f.r2 == Approx(1.0));
}

TEST_CASE("power-law fit ignores non-positive points") {
    std::vector<double> x = {-1, 0, 1, 2, 4};
    std::vector<double> y = {5, 5, 2, 2 * std::sqrt(2.0), 4};  // y=2*sqrt(x) on the valid tail
    auto                f = fit_power_law(x, y);
    CHECK(f.n == 3);  // only x=1,2,4 used
    CHECK(f.exponent == Approx(0.5));
    CHECK(f.prefactor == Approx(2.0));
}
