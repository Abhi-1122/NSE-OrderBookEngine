# NSE Order Book Engine — Build Plan

> **Status: In Progress**

***

## What This Project Is

A **low-latency C++ order book engine for NSE/Nifty market data** — a system that ingests raw tick-level trade data, maintains a live representation of the bid and ask sides of the market at nanosecond-benchmarked speed, and surfaces market microstructure analytics on top.

This is the class of infrastructure that sits at the core of every HFT and quant trading firm's technology stack. This is a simplified but technically correct and benchmarked implementation of it, built on real NSE/Nifty 50 data.

***

## Planned Architecture

```
NSE Historical CSV (Nifty 50 tick data)
            ↓
      Feed Parser (C++)
      Converts rows → OrderEvent structs
      Integer paise pricing, #pragma pack(1)
            ↓
   Order Book Engine (C++)
   std::map bids/asks (Phase 2)
   → Flat Price Ladder (Phase 3 optimization)
   order_map: unordered_map<order_id → {side, price, qty}>
   for O(1) delete and execute
            ↓
      rdtsc Benchmarking
      P50 / P99 / P999 latency
      Before (std::map) and After (flat ladder)
            ↓
      CSV Snapshot Export
      TopOfBook at every tick event
            ↓
      Python Analytics (pandas)
      Mid price · Bid-ask spread · Queue imbalance
            ↓
      Streamlit Dashboard
      Live replay · Order book depth · Latency table
```

***

## Planned Stack

| Layer | Technology |
|---|---|
| Order book engine | C++17 |
| Benchmarking | `__rdtsc()`, `perf stat` |
| Data source | NSE historical CSV (Nifty 50) |
| Analytics | Python, pandas, matplotlib |
| Dashboard | Streamlit, Plotly |
| Build | CMake / Makefile |

***

## Key Design Decisions (Planned)

**Integer paise pricing, not `double`**
NSE prices move in ₹0.05 ticks. All prices stored as `int64_t` paise (₹ × 100). Floating point arithmetic is non-associative — `(a + b) + c ≠ a + (b + c)` in IEEE 754. Exact integer arithmetic is required for correct financial computation.

**`order_map` alongside the order book**
Delete and Execute events carry only an `order_id`, not a price. Without a side-table mapping `order_id → {side, price, qty}`, processing a delete requires scanning the book. With `order_map`, delete and execute are O(1) hash lookups followed by a single map removal.

**Flat price ladder over `std::map`**
`std::map` is a red-black tree — O(log n), pointer-chasing, cache-unfriendly. A pre-allocated array indexed by price level (`(price - min_price) / 5`) is O(1) and cache-sequential. This is the optimization that drops P99 from ~300–400ns to ~60–100ns.

**`rdtsc` over `clock_gettime`**
`clock_gettime` has ~20–30ns syscall overhead — that overhead alone dominates sub-100ns measurements. `__rdtsc()` reads the CPU hardware time-stamp counter in a single instruction with no kernel involvement.

***

## 6-Phase Build Plan

### Phase 1 — Foundation: Structs + Data Generator
- [ DONE ] Define `OrderEvent` struct with `#pragma pack(1)`, paise pricing, all fields
- [ DONE ] Define `TopOfBook` struct
- [ DONE ] Write synthetic NSE-calibrated data generator (Poisson arrivals, ₹0.05 ticks)
- [ DONE ] Write NSE CSV reader producing `OrderEvent` (same interface as synthetic generator)

**Exit condition:** 1000 events generated and printed. Every price is a clean multiple of 5 paise. Zero floats.

***

### Phase 2 — Order Book Core: `std::map` Version
- [ DONE ] Implement `OrderBook` with `std::map` bids (descending) + asks (ascending)
- [ DONE ] Implement `order_map: unordered_map<order_id → {side, price, qty}>`
- [ DONE ] Implement `add()`, `remove()`, `execute()`, `cancel()`, `snapshot()`
- [ DONE ] Unit tests: 100 adds → 50 deletes → 20 executes → verify all sides correct
- [ DONE ] Edge case tests: partial execute, level disappears when qty hits 0, crossed book prevention

**Exit condition:** All correctness tests pass. Book state manually verifiable after mixed event sequences.

***

### Phase 3 — Benchmarking + Flat Price Ladder
- [ DONE ] Integrate `rdtsc` benchmarking wrapper, compute P50 / P99 / P999 over 1M events
- [ DONE ] Record baseline `std::map` latency numbers
- [ DONE ] Implement `PriceLadder`: pre-allocated `int32_t qty[LEVELS]`, indexed by `(price - min_price) / 5`
- [ DONE ] Swap `std::map` for `PriceLadder` (external API unchanged)
- [ DONE ] Re-benchmark: record improved P50 / P99 / P999

**Exit condition:** Before/after benchmark table in hand. P99 drop from ~300–400ns (`std::map`) to ~60–100ns (flat ladder).

## Benchmark Results after implementing phase 3
*1M events on Nifty 50 synthetic data, Intel CPU @ 4.7GHz boost, taskset -c 0*

| Implementation     | P50      | P99      | P99.9    | Throughput     |
|--------------------|----------|----------|----------|----------------|
| `std::map`         | 88.5 ns  | 322.1 ns | 564.3 ns | 9.8M ops/sec   |
| Flat price ladder  | 12.8 ns  | **92.8 ns**  | 122.6 ns | **49.5M ops/sec** |
| **Speedup**        | **6.9x** | **3.5x** | **4.6x** | **5.1x**       |

***

### Phase 4 — Pipeline + CSV Export
- [ ] Wire main loop: NSE CSV → parser → order book → `snapshot()` → output CSV
- [ ] Output CSV columns: `timestamp_ns, best_bid_price, best_bid_qty, best_ask_price, best_ask_qty`
- [ ] Run on real NSE Nifty 50 historical session
- [ ] Sanity check: bid always below ask, spread 1–3 ticks for liquid instruments

**Exit condition:** Valid output CSV from a full NSE trading session.

***

### Phase 5 — Python Analytics
- [ ] Mid price: `(best_bid + best_ask) / 2`
- [ ] Bid-ask spread: `best_ask - best_bid` in rupees
- [ ] Queue imbalance: `(bid_qty - ask_qty) / (bid_qty + ask_qty)`
- [ ] Time series plots for all three signals
- [ ] Summary stats: mean spread, spread distribution histogram, imbalance autocorrelation

**Exit condition:** Three interpretable plots from a real Nifty session. Spread tighter 10am–2pm IST, wider at open/close.

***

### Phase 6 — Streamlit Dashboard + README
- [ ] Dashboard: mid price + spread band, order book depth bars, queue imbalance, order flow stats, benchmark table
- [ ] Replay mode: scrub through trading session via slider
- [ ] Architecture diagram (Excalidraw → PNG)
- [ ] Final README: architecture, benchmark table, setup instructions, queue imbalance explanation
- [ ] 60-second screen capture → GIF embedded in README

**Exit condition:** Clone → two commands → dashboard running on Nifty data.

***

## Planned Analytics

**Queue Imbalance** — `(bid_qty - ask_qty) / (bid_qty + ask_qty)` — ranges from -1 (full sell pressure) to +1 (full buy pressure). A well-documented short-term directional signal in equity microstructure (Cont et al., 2014). Positive imbalance predicts upward price movement over the next few ticks with statistically significant frequency.

**Bid-Ask Spread** — direct liquidity measure. Tight spread = institutional participation, deep book. Wide spread = uncertainty or thin market. Plotting intraday spread on Nifty 50 reveals characteristic patterns around market open, lunch hour, and close.

**Mid Price** — `(best_bid + best_ask) / 2` — the best estimate of fair value at each tick, more stable than last trade price.
