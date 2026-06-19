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

## FlatBook: the optimized engine

`FlatBook` keeps the same semantics as `OrderBook` but swaps the two data
structures that dominate the hot path:

- **Price levels:** `std::map<Price, Level>` per side -> a flat `std::vector`
  per side indexed by tick offset from a fixed base. O(1) price access and no
  red-black-tree pointer chasing. The inside is tracked incrementally; when it
  empties, a short scan finds the next occupied level.
- **Order nodes:** map nodes -> a pooled `std::vector<Node>` with a free list and
  index links, so steady-state add/cancel does no heap allocation.

**Bids and asks need separate price arrays.** A single shared array breaks the
instant the book transiently crosses (an ask at or below a bid, which real raw
event streams produce): one `Level` per price cannot hold both sides, so the
best-ask scan can land on a bid level. This was caught by the differential test;
the fix is per-side arrays.

### Benchmark (`bench`, apply loop only, I/O excluded, best of 9)

AAPL 2012-06-21, GCC 16 `-O3`, replaying the real message stream:

| dataset | events | price band | `std::map` | `FlatBook` | speedup |
|---|---|---|---|---|---|
| level-10 full day | 400,391 | 109,401 ticks | 4.5 M/s | 6.8 M/s | **1.50x** |
| level-50 one hour | 91,997 | 2,219,501 ticks | 3.9 M/s | 4.6 M/s | 1.18x |

Both engines' best quotes are checksummed event-for-event and **match exactly**,
so the speedup comes with a correctness proof against the validated baseline.

Two honest caveats this exposes:

- The remaining shared cost is the `OrderId`-to-node hash map, hit on every
  reduce/cancel by *both* engines; it compresses the achievable ratio. Beating
  it needs a different id-lookup scheme, a separate axis of work.
- A fixed flat array degrades when the price band is wide (the level-50 sample
  has deep far-from-mid limit orders spanning ~2.2M ticks): mostly-empty memory
  and longer inside-scan gaps erode the win. A windowed / recentering array is
  the natural fix and a candidate next step.

## Microstructure analytics

`apps/microstructure.cpp` seeds deep from the first snapshot, replays the feed,
and measures the canonical empirical observables; the estimator math lives in the
header-only [microstructure.hpp](../include/lob/microstructure.hpp) and is unit
tested against hand-computed answers. Trade sign follows eps = -Direction;
same-timestamp same-sign executions are aggregated into one trade.

On the AAPL 2012-06-21 level-50 hour (91,996 events, 6,268 executions -> 4,575
trades) it recovers the textbook stylized facts:

- **Long-memory order flow.** Trade-sign autocorrelation C(1) = 0.58 decaying to
  ~0.10 by lag 10 and ~0 by lag 50 -- the slow, persistent sign correlation.
- **Concave, saturating impact.** Response R(1) = 197 ticks rising to R(10) = 541
  and flattening by R(50), the canonical shape.
- **Effective < quoted spread.** 2.1 bps effective vs 3.3 bps time-weighted
  quoted -- trades occur inside the quotes, as they must.
- **Single-trade impact is weakly size-dependent** (~150-280 ticks across three
  decades of trade size), consistent with the literature: the square-root law is
  a *metaorder*-level effect, not a single-trade one. That is milestone 5.

Outputs `response_function.csv`, `sign_autocorrelation.csv`, `impact_by_size.csv`
for plotting. The time-weighted quoted spread is sensitive to the ~0.1% of
snapshots that reconstruction gets wrong (a stale wide level over a quiet
interval is heavily time-weighted); the trade-based effective spread is robust.

## The square-root law (Maitrier-Loeper-Bouchaud)

`apps/square_root_law.cpp` reproduces the square-root law of market impact on a
reconstructed book, via the construction of Maitrier, Loeper & Bouchaud
([arXiv:2503.18199](https://arxiv.org/abs/2503.18199)). Real metaorder studies
need proprietary per-trader data; MLB's insight is that the law does not depend
on *which* trader sends what, so it can be recovered from the anonymous public
tape by a random mapping:

1. Reconstruct the trade tape (`lobster_tape`): child orders = same-timestamp
   same-sign execution bursts, each with its pre- and post-trade mid.
2. Assign every trade to one of N synthetic traders at random, preserving
   chronological order (the MLB mapping function).
3. A metaorder is a maximal same-sign run within one trader's sequence, keeping
   only runs of more than one child order.
4. For volume Q: impact I = eps * (ln p_e - ln p_s), p_s the mid before the first
   child, p_e the mid after the last.
5. Average over many random mappings, bin by Q / V_D, fit
   I(Q) / sigma_D = Y * (Q / V_D)^delta.

**Result, AAPL 2012-06-21 level-50 hour** (4,575 child orders, 20 traders, 500
mappings -> 553k metaorders):

- Impact is **strongly concave**, ruling out linear (Kyle) impact outright.
- **Small-Q plateau:** below Q/V_D ~ 3e-4 impact flattens at the spread /
  discreteness floor (the smallest metaorders all cross about one spread). MLB
  exclude this region; a single fit across all scales is dragged down to an
  exponent of 0.22 by it.
- **Scaling regime** (Q/V_D >= 3e-4, high-mass bins): `I/sigma_D = 4.6 *
  (Q/V_D)^0.62`, **R^2 = 0.97** -- a clean concave power law consistent with the
  square-root law (delta = 0.5).

The 0.62 vs the asymptotic 0.5 is the honest cost of one hour of data: MLB obtain
clean four-decade scaling from multi-year datasets, and the exponent here wanders
~0.4-0.6 with the fit window. What is unambiguous is the concavity and the
recovery of square-root-like scaling **from a book reconstructed out of a raw
exchange feed** -- the same measurement an impact study runs, now end to end in
this engine. `square_root_law.csv` holds the full impact curve for plotting.

## Roadmap

1. **[done]** Reconstruction core + hand-written known-answer unit tests.
2. **[done]** LOBSTER loader + oracle test (this section), with `reduce_at` and
   deep-seed / shallow-validate support.
3. **[done]** Flat-array book ([flat_book.hpp](../include/lob/flat_book.hpp)) +
   object pool; benchmark vs the `std::map` baseline (below).
4. **[done]** Microstructure measurements off the reconstructed book (below).
5. **[done]** Metaorder construction (Maitrier-Loeper-Bouchaud) and the
   square-root law (below).
6. Optional: matching-engine mode with price-time priority.
