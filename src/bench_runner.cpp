#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>

#include "../include/types.h"
#include "synthetic.h"
#include "orderbook.h"
#include "price_ladder.h"
#include "bench.h"

// ── Benchmark runner ──────────────────────────────────────────────────────────
//
// Runs identical 1M-event workloads through:
//   1. OrderBook      (std::map  — Phase 2)
//   2. PriceLadderBook (flat array — Phase 3)
//
// Prints before/after comparison table.
//
// Methodology notes (know these for interviews):
//
//   - We measure only the hot-path operation (add/remove/execute/cancel)
//     with lfence on either side to prevent CPU reordering of rdtsc.
//
//   - The first 10K events are a warm-up run — discarded. This lets the
//     CPU branch predictor and instruction cache warm up so we measure
//     steady-state performance, not cold-start behaviour.
//
//   - We pre-generate all events before the benchmark loop so that the
//     random number generator cost is not included in latency samples.
//
//   - We alternate add/non-add events in the workload. Benchmarking only
//     add() would be misleading — real workloads are mixed. However we
//     record latency for every event type separately so you can report
//     add-specific P99 if asked.

static constexpr int WARMUP = 10'000;
static constexpr int MEASURE = 1'000'000;
static constexpr int TOTAL = WARMUP + MEASURE;
double CPU_GHZ = 4.7;

// Pre-generate the benchmark event stream
std::vector<OrderEvent> generate_workload(int n)
{
    SyntheticGenerator::Config cfg;
    cfg.ref_price_paise = 22500'00;
    cfg.price_std_paise = 100'00; // ±₹100 spread — realistic Nifty range
    cfg.arrival_rate = 500.0;
    SyntheticGenerator gen(cfg);
    return gen.generate(n);
}

// ── Benchmark OrderBook (std::map) ────────────────────────────────────────────
void bench_stdmap(const std::vector<OrderEvent> &events, Bench &b)
{
    OrderBook book;
    b.clear();

    // warm-up — not recorded
    for (int i = 0; i < WARMUP; ++i)
    {
        // silently skip duplicate order_ids in warm-up
        if (!book.has_order(events[i].order_id) || events[i].msg_type != 'A')
            book.apply(events[i]);
    }

    // hot path — NO try/catch, no branches beyond apply()
    for (int i = WARMUP; i < TOTAL; ++i)
    {
        b.start();
        book.apply(events[i]);
        b.stop();
    }
}

// ── Benchmark PriceLadderBook (flat array) ────────────────────────────────────
void bench_ladder(const std::vector<OrderEvent> &events, Bench &b)
{
    PriceLadderBook::Config cfg;
    cfg.ref_price_paise = 22500'00;
    cfg.band_pct = 0.05;
    PriceLadderBook book(cfg);
    b.clear();

    for (int i = 0; i < WARMUP; ++i)
    {
        if (!book.has_order(events[i].order_id) || events[i].msg_type != 'A')
            book.apply(events[i]);
    }

    for (int i = WARMUP; i < TOTAL; ++i)
    {
        b.start();
        book.apply(events[i]);
        b.stop();
    }
}

// ── Comparison table printer ──────────────────────────────────────────────────
void print_comparison(const Bench &map_bench, const Bench &ladder_bench)
{
    double map_p50 = map_bench.p50_ns();
    double map_p99 = map_bench.p99_ns();
    double map_p999 = map_bench.p999_ns();
    double lad_p50 = ladder_bench.p50_ns();
    double lad_p99 = ladder_bench.p99_ns();
    double lad_p999 = ladder_bench.p999_ns();

    auto speedup = [](double before, double after) -> double
    {
        return (after > 0.0) ? before / after : 0.0;
    };

    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║          ORDER BOOK BENCHMARK — BEFORE / AFTER              ║\n");
    printf("║                  %7d measured events                    ║\n", MEASURE);
    printf("╠══════════════════╦══════════════════╦═══════════════════════╣\n");
    printf("║  Metric          ║  std::map        ║  Flat Price Ladder    ║\n");
    printf("╠══════════════════╬══════════════════╬═══════════════════════╣\n");
    printf("║  P50  latency    ║  %8.1f ns     ║  %8.1f ns  (%.1fx)  ║\n",
           map_p50, lad_p50, speedup(map_p50, lad_p50));
    printf("║  P99  latency    ║  %8.1f ns     ║  %8.1f ns  (%.1fx)  ║\n",
           map_p99, lad_p99, speedup(map_p99, lad_p99));
    printf("║  P99.9 latency   ║  %8.1f ns     ║  %8.1f ns  (%.1fx)  ║\n",
           map_p999, lad_p999, speedup(map_p999, lad_p999));
    printf("╠══════════════════╩══════════════════╩═══════════════════════╣\n");
    printf("║  → Put the P99 flat-ladder number on your resume            ║\n");
    printf("║  → Speedup is the interview talking point                   ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n");
    printf("\n");
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main()
{
    printf("════════════════════════════════════════════════════════════════\n");
    printf("  NSE Order Book — Phase 3 Benchmark\n");
    printf("  Workload: %dK warm-up + %dK measured events\n",
           WARMUP / 1000, MEASURE / 1000);
    printf("════════════════════════════════════════════════════════════════\n\n");

    // ── Print PriceLadder config ───────────────────────────────────────────
    {
        PriceLadderBook probe;
        probe.print_config();
        printf("\n");
    }

    // ── Pre-generate event stream ─────────────────────────────────────────
    printf("Generating %d events... ", TOTAL);
    fflush(stdout);
    auto events = generate_workload(TOTAL);
    printf("done.\n\n");

    // ── Run std::map benchmark ────────────────────────────────────────────
    printf("Running std::map benchmark (%dK events)...\n", MEASURE / 1000);
    Bench map_bench("OrderBook (std::map)", CPU_GHZ);
    bench_stdmap(events, map_bench);
    map_bench.report();

    // ── Run flat ladder benchmark ─────────────────────────────────────────
    printf("\nRunning flat price ladder benchmark (%dK events)...\n", MEASURE / 1000);
    Bench ladder_bench("PriceLadderBook (flat array)", CPU_GHZ);
    bench_ladder(events, ladder_bench);
    ladder_bench.report();

    // ── Before/after comparison ───────────────────────────────────────────
    print_comparison(map_bench, ladder_bench);

    // ── Correctness spot-check ────────────────────────────────────────────
    // Run both books on the same 1000-event sequence and verify snapshots match
    printf("Running correctness cross-check (1000 events, std::map vs flat array)...\n");
    {
        auto check_events = generate_workload(1000);
        OrderBook ref_book;
        PriceLadderBook fast_book;
        int mismatches = 0;

        for (const auto &e : check_events)
        {
            try
            {
                ref_book.apply(e);
            }
            catch (...)
            {
            }
            try
            {
                fast_book.apply(e);
            }
            catch (...)
            {
            }

            TopOfBook ref = ref_book.snapshot();
            TopOfBook fast = fast_book.snapshot();

            if (ref.best_bid_price != fast.best_bid_price ||
                ref.best_ask_price != fast.best_ask_price ||
                ref.best_bid_qty != fast.best_bid_qty ||
                ref.best_ask_qty != fast.best_ask_qty)
            {
                ++mismatches;
                if (mismatches <= 3)
                {
                    printf("  MISMATCH at event %llu:\n", (unsigned long long)e.order_id);
                    printf("    ref:  bid=₹%lld.%02lld qty=%d  ask=₹%lld.%02lld qty=%d\n",
                           (long long)(ref.best_bid_price / 100), (long long)(ref.best_bid_price % 100),
                           ref.best_bid_qty,
                           (long long)(ref.best_ask_price / 100), (long long)(ref.best_ask_price % 100),
                           ref.best_ask_qty);
                    printf("    fast: bid=₹%lld.%02lld qty=%d  ask=₹%lld.%02lld qty=%d\n",
                           (long long)(fast.best_bid_price / 100), (long long)(fast.best_bid_price % 100),
                           fast.best_bid_qty,
                           (long long)(fast.best_ask_price / 100), (long long)(fast.best_ask_price % 100),
                           fast.best_ask_qty);
                }
            }
        }
        if (mismatches == 0)
        {
            printf("  ✓ All 1000 snapshots match between std::map and flat array\n\n");
        }
        else
        {
            printf("  ✗ %d mismatches found — check price_ladder.hpp reduce() logic\n\n",
                   mismatches);
        }
    }

    printf("Phase 3 complete.\n");
    printf("Put the flat-ladder P99 number in your README benchmark table.\n\n");

    return 0;
}