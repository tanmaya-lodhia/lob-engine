#pragma once

#include <cstdint>
#include <limits>

namespace lob {

// Price is stored as an integer number of ticks, never as floating point.
// LOBSTER quotes prices in units of 1e-4 USD (i.e. dollar price * 10000), so a
// price already arrives as an integer and we keep it that way. Using ints
// removes any rounding ambiguity when prices are used as map keys, which is the
// single most common source of silent bugs in order-book code.
using Price    = std::int64_t;
using Quantity = std::int64_t;  // shares (LOBSTER sizes are integers)
using OrderId  = std::int64_t;

enum class Side : std::uint8_t { Buy, Sell };

// Sentinel returned when a side of the book is empty.
inline constexpr Price kNoPrice = std::numeric_limits<Price>::min();

constexpr Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

}  // namespace lob
