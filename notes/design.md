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

## Roadmap

1. **[done]** Reconstruction core + hand-written known-answer unit tests.
2. LOBSTER loader + **oracle test**: replay a message file and assert the
   reconstructed top-N levels equal LOBSTER's published orderbook file at every
   event.
3. Flat-array book + object pool; microbenchmark events/sec vs the `std::map`
   baseline.
4. Microstructure measurements off the reconstructed book: realized spread,
   depth, queue dynamics, and the square-root impact law on real metaorders
   (ties into the impact-diffusivity work).
5. Optional: matching-engine mode with price-time priority.
