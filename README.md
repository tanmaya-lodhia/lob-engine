# LOB Engine

A limit order book reconstruction engine in C++23. It reads an exchange's raw message stream (every add, cancel and execution) and rebuilds the full book after each event. The reason to do this in C++ is volume: a single stock can throw off tens to hundreds of millions of events in a day, and you touch the price-level structure on every one. That's the kind of loop where a scripting language falls over and where controlling the memory layout actually buys you something.

The data is [LOBSTER](https://lobsterdata.com) NASDAQ samples. I picked it because it ships the true book state after every message, so the reconstruction can be checked against an external source instead of just my own tests.

## What's in here

The core is the order book (`order_book.hpp`): integer-tick prices, a FIFO queue per price level threaded as an intrusive linked list so any order cancels in O(1), and an order-id map for lookups. It keeps strict price-time priority, which is the one thing the whole exercise has to get right.

A few tools sit on top of that core:

- **replay** runs a message file through the book and prints per-day stats.
- **oracle** is the real validation. It seeds the book from LOBSTER's first snapshot, replays the messages, and compares the result to LOBSTER's own book at every single event. On the level-50 AAPL sample the top of book matches 99.9% of the time, and is exact for the first 54,605 events in a row. The rest isn't a bug. A level-N feed doesn't report what happens below level N, so deep liquidity occasionally disappears with no message to tell you. The design notes walk through a specific case where this happens.
- **microstructure** measures the standard empirical facts off the rebuilt book: time-weighted spread and depth, the trade-sign autocorrelation, and the impact response function R(ℓ). The AAPL hour reproduces the textbook results: sign autocorrelation around 0.58 decaying to zero, and a concave response curve that saturates.
- **square_root_law** reproduces the square-root law of market impact using the Maitrier-Loeper-Bouchaud construction ([arXiv:2503.18199](https://arxiv.org/abs/2503.18199)): randomly assign trades to synthetic traders, treat each same-sign run as a metaorder, and plot impact against volume. Over the scaling region the fit is I/σ_D proportional to (Q/V_D)^0.62 with R² of 0.97. That's concave and clearly sub-linear, in line with the square-root law. The exponent isn't exactly 0.5 because this is one hour of data rather than the years the original paper used, but the shape is unambiguous.

Two more pieces sit alongside the reconstruction path:

- **flat_book** is the same book with the `std::map` levels swapped for flat per-side arrays and the map nodes for a pooled free list. It runs about 1.5x faster on the real stream. The benchmark checksums both books' quotes on every event, so it confirms they agree before reporting the speedup. Building it I hit a good bug: one shared price array can't represent a book that transiently crosses, and the checksum caught it.
- **matching engine** is the opposite of reconstruction. Instead of replaying matches it makes them, crossing incoming orders by price-time priority: limit and market orders, with GTC, IOC and FOK. It reuses the order book for storage so it leans on the same tested code.

Tests are doctest: invariants on both books, a 50k-step fuzz test that pushes random order flow through the flat book and the `std::map` book and checks they never disagree (including crossed books), and a fixture that demands a bit-exact reconstruction on a clean feed.

There's more detail in `notes/design.md`: the data-structure choices, the exact LOBSTER event mapping, the depth limitation above, and the full results.

## Structure

```
include/lob/   headers: order book, flat book, matching engine, tape, microstructure
src/           implementations
apps/          replay, oracle, microstructure, square_root_law, match_demo
bench/         std::map vs flat-array benchmark
tests/         doctest unit tests, fuzz test, oracle fixture
notes/         design notes and results
data/lobster/  samples (gitignored)
```

## Building

Needs a C++23 compiler, CMake 3.24+ and Ninja. Built with GCC 16 (MinGW-w64 UCRT) on Windows.

```
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=g++
cmake --build build
ctest --test-dir build --output-on-failure
```

Grab a free LOBSTER sample and run the tools on it:

```
python scripts/download_lobster_sample.py --levels 50

# per-day summary stats
build/replay data/lobster/AAPL_2012-06-21_34200000_37800000_message_50.csv

# validate against LOBSTER's own book: seed 50 levels deep, check the top of book
build/oracle \
  data/lobster/AAPL_2012-06-21_34200000_37800000_message_50.csv \
  data/lobster/AAPL_2012-06-21_34200000_37800000_orderbook_50.csv \
  50 1

# benchmark the flat book against the std::map baseline (best of 9)
build/bench data/lobster/AAPL_2012-06-21_34200000_57600000_message_10.csv 9

# microstructure stats, lags 1..100, CSVs into ./out
build/microstructure \
  data/lobster/AAPL_2012-06-21_34200000_37800000_message_50.csv \
  data/lobster/AAPL_2012-06-21_34200000_37800000_orderbook_50.csv \
  50 100 out

# square-root law: 20 synthetic traders, 500 random mappings
build/square_root_law \
  data/lobster/AAPL_2012-06-21_34200000_37800000_message_50.csv \
  data/lobster/AAPL_2012-06-21_34200000_37800000_orderbook_50.csv \
  50 20 500 out
```

## Author

Tanmaya Lodhia, University of Nottingham
