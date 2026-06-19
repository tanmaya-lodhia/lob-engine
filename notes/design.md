# Design notes

## What this engine is (and is not)

It is a **limit-order-book reconstruction engine**: it consumes a stream of
already-matched market events (the LOBSTER / exchange convention) and rebuilds
the full-depth book after every event. It is **not** (yet) a matching engine —
incoming events describe what the exchange already decided, so the engine never
crosses the book itself.

This distinction matters and is deliberate:

- **Reconstruction** can be validated *exactly* against an external oracle
  (LOBSTER publishes its own book snapshot after every message), which gives us
  a rigorous known-answer test on real data rather than only hand-built unit
  tests.
- **Matching** (price-time priority crossing of marketable orders) is a
  separate, clearly-scoped module planned next. It shares the same `OrderBook`
  data structures but adds a `match()` path. Keeping it separate means the
  reconstruction correctness story stays clean.

## Data-structure choices

| Concern | Choice | Why |
|---|---|---|
| Price | `int64` ticks, never float | prices are map keys; float keys are the classic silent-bug source. LOBSTER prices are already integers (1e-4 USD). |
| Price levels | `std::map<Price, Level>` per side | obviously-correct ordered baseline; best bid = `rbegin`, best ask = `begin`. |
| Queue at a level | intrusive doubly linked list | O(1) cancel of an arbitrary order; preserves FIFO time priority. |
| Order lookup | `std::unordered_map<OrderId, Order>` | O(1) cancel/reduce by id. Node addresses are stable across rehash, so the intrusive `Order*` pointers stay valid. |

The `std::map` baseline is intentionally the *reference*, not the final answer.
The planned optimization is a flat array of price levels indexed by tick offset
around the inside, plus an object pool for `Order` nodes — replacing the
per-event `map` lookups and node allocations that dominate the profile. That
optimization lives **behind the same `OrderBook` interface** and is benchmarked
against this baseline, so the speedup is a measured result, not an assertion.

## LOBSTER event mapping

| LOBSTER type | Meaning | Engine call |
|---|---|---|
| 1 | new visible limit order | `add_limit` |
| 2 | partial cancellation | `reduce` |
| 3 | full deletion | `cancel` |
| 4 | execution of visible order | `reduce` (+ record trade) |
| 5 | execution of hidden order | no book change (+ record trade) |
| 6 | cross / auction | ignored |
| 7 | trading halt | ignored |

## The depth limitation of a level-N feed

LOBSTER sells data at a fixed depth N (10, 50, ...). The message file contains
only events that touch the top N levels; activity below level N is **not
reported**. This has a sharp, easily-misread consequence for reconstruction.

Worked example from the level-10 AAPL sample. The seed book has an ask of
1160 shares at price 587.65 sitting at level 9. Three transient orders then join
the book at better ask prices, pushing 587.65 down to ~level 12 — below the
reported depth. While it is invisible, it is removed; that removal is **not in
the message file** because it happened below level 10. When the three transient
orders later cancel, the true book reveals a different level-10 price than our
engine holds: we never learned 587.65 was gone. The error then ratchets upward
over the day as the price wanders, eventually corrupting even the top of book.

This is not an engine bug — it is information the feed does not contain. Two
consequences shape the validation:

- **Seed deeper than you validate.** Seeding 50 levels and checking only the top
  buffers the inside quote against sub-depth loss for far longer than a 10-level
  seed does (see results below).
- **Self-contained feeds reconstruct exactly.** When no activity ever crosses
  the depth boundary (the regression fixture, or a true full-depth feed),
  reconstruction is bit-exact at every level. That is the strict CI test.

## Oracle validation results

`oracle.cpp` seeds from the first published snapshot, replays the message file,
and compares the reconstructed top-N to LOBSTER's orderbook file at every event.
By-id reduce/cancel is used for in-window orders; the `reduce_at` price-level
fallback handles events on pre-seed liquidity.

- **Regression fixture** (`tests/fixtures/oracle_*.csv`, no sub-depth activity):
  100% exact at all levels — the `oracle_fixture` ctest.
- **AAPL 2012-06-21, level-50 sample (~92k events, 09:30–10:30):** seeding 50
  levels and validating the top of book gives **99.90%** snapshot agreement,
  exact for the first 54,605 consecutive events. Cumulative agreement is 97.5%
  out to the full 10 levels. The residual is precisely the sub-depth loss above.
- **AAPL level-10 full day:** top of book is exact only for the first ~440
  events before sub-depth loss reaches the inside — the same effect, with far
  less depth to buffer it. This is why deeper seeding matters.

## Roadmap

1. **[done]** Reconstruction core + hand-written known-answer unit tests.
2. **[done]** LOBSTER loader + oracle test (this section), with `reduce_at` and
   deep-seed / shallow-validate support.
3. Flat-array book + object pool; microbenchmark events/sec vs the `std::map`
   baseline. (Oracle throughput is currently bounded by CSV parsing, not the
   book; a dedicated benchmark will isolate the engine.)
4. Microstructure measurements off the reconstructed book: realized spread,
   depth, queue dynamics, and the square-root impact law on real metaorders
   (ties into the impact-diffusivity work).
5. Optional: matching-engine mode with price-time priority.
