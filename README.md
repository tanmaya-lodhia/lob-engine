# LOB Engine

A from-scratch, full-depth **limit-order-book reconstruction engine** in modern
C++ (C++23). It ingests an exchange's message-by-message event stream and
rebuilds the complete book after every event, fast enough to replay full trading
days that are impractical to process in Python.

The first data target is [LOBSTER](https://lobsterdata.com) NASDAQ message data,
chosen because it ships a reference book snapshot after every message — so
reconstruction can be validated *exactly* against an external oracle, not just
hand-written tests.

## Why C++

A single equity name can generate tens to hundreds of millions of book events in
one session. Reconstruction means an add / cancel / execute against a price-level
structure *per event*, and depth queries on top. The hot path is dominated by
data-structure access and allocation — exactly where manual control over memory
layout matters and a per-event interpreted loop does not keep up.

## What it does

- **Order-book core** ([order_book.hpp](include/lob/order_book.hpp),
  [order_book.cpp](src/order_book.cpp)): integer-tick prices, one FIFO queue per
  price level threaded as an intrusive linked list (O(1) cancel of any order),
  and an order-id index for O(1) lookup. Best bid/ask, depth-at-price, and
  top-N snapshots. Preserves price-time priority — the invariant the whole
  engine exists to protect.
- **Replay driver** ([replay.cpp](apps/replay.cpp)): maps the LOBSTER event
  encoding (new / cancel / delete / execute / hidden) onto the engine and emits
  per-day summary statistics.
- **Known-answer tests** ([test_order_book.cpp](tests/test_order_book.cpp)):
  the core invariants pinned with doctest (vendored, no network at build time).

See [notes/design.md](notes/design.md) for data-structure rationale, the exact
LOBSTER event mapping, and the roadmap (oracle test against LOBSTER's own book
file, flat-array optimization with benchmarks, microstructure measurements).

This is a **reconstruction** engine, not a matching engine: events are already
matched by the exchange, so the book is never crossed here. A matching mode is a
planned, separately tested module.

## Structure

```
include/lob/   public headers: types, order book
src/           order-book implementation
apps/          replay driver (LOBSTER message file -> stats)
tests/         known-answer unit tests (doctest)
third_party/   vendored doctest single header
scripts/       data fetch helper
notes/         design notes and roadmap
data/lobster/  sample data (gitignored; not committed)
```

## Building

Requires a C++23 compiler, CMake >= 3.24, and Ninja. Verified with GCC 16
(MinGW-w64 UCRT) on Windows.

```
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=g++
cmake --build build
ctest --test-dir build --output-on-failure
```

Run the replay driver on a LOBSTER message file:

```
build/replay data/lobster/AAPL_2012-06-21_message_10.csv
```

## Author

Tanmaya Lodhia, University of Nottingham
