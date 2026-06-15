#include <cstdio>
#include <cstring>
#include <string>
#include <fstream>
#include <sstream>
#include <chrono>
#include <vector>
#include <stdexcept>
#include <sys/stat.h>

#include "../include/types.h"
#include "csv_reader.h"
#include "price_ladder.h" // use the fast book for the pipeline

// ── Pipeline stats ────────────────────────────────────────────────────────────

struct PipelineStats
{
    uint64_t total_events = 0;
    uint64_t add_count = 0;
    uint64_t delete_count = 0;
    uint64_t execute_count = 0;
    uint64_t cancel_count = 0;
    uint64_t skipped = 0;        // events where book had no valid snapshot
    uint64_t invalid_spread = 0; // bid >= ask (data quality issue)
    double min_spread_rs = 1e9;
    double max_spread_rs = 0.0;
    double sum_spread_rs = 0.0;
    uint64_t spread_samples = 0;

    void record_event(const OrderEvent &e)
    {
        ++total_events;
        switch (e.msg_type)
        {
        case 'A':
            ++add_count;
            break;
        case 'D':
            ++delete_count;
            break;
        case 'E':
            ++execute_count;
            break;
        case 'X':
            ++cancel_count;
            break;
        }
    }

    void record_snapshot(const TopOfBook &tob)
    {
        if (!tob.valid())
        {
            ++skipped;
            return;
        }

        double bid = tob.best_bid_price / 100.0;
        double ask = tob.best_ask_price / 100.0;
        double sp = ask - bid;

        if (sp <= 0)
        {
            ++invalid_spread;
            return;
        }

        ++spread_samples;
        sum_spread_rs += sp;
        if (sp < min_spread_rs)
            min_spread_rs = sp;
        if (sp > max_spread_rs)
            max_spread_rs = sp;
    }

    void print() const
    {
        printf("\n── Pipeline Stats ───────────────────────────────────────\n");
        printf("  Total events processed:  %llu\n", (unsigned long long)total_events);
        printf("    Add:     %llu\n", (unsigned long long)add_count);
        printf("    Delete:  %llu\n", (unsigned long long)delete_count);
        printf("    Execute: %llu\n", (unsigned long long)execute_count);
        printf("    Cancel:  %llu\n", (unsigned long long)cancel_count);
        printf("  Snapshots written:       %llu\n",
               (unsigned long long)(spread_samples + invalid_spread));
        printf("  Skipped (empty book):    %llu\n", (unsigned long long)skipped);
        printf("  Invalid spread (bid>=ask):%llu\n", (unsigned long long)invalid_spread);
        if (spread_samples > 0)
        {
            printf("  Spread — min: ₹%.2f  max: ₹%.2f  avg: ₹%.4f\n",
                   min_spread_rs, max_spread_rs,
                   sum_spread_rs / (double)spread_samples);
        }
        printf("────────────────────────────────────────────────────────\n");
    }
};

// ── TopOfBook CSV writer ───────────────────────────────────────────────────────

class SnapshotWriter
{
public:
    explicit SnapshotWriter(const std::string &path)
    {
        file_ = fopen(path.c_str(), "w");
        if (!file_)
            throw std::runtime_error("Cannot open output file: " + path);

        // Write header
        fprintf(file_,
                "timestamp_ns,"
                "symbol,"
                "best_bid_price,"
                "best_bid_qty,"
                "best_ask_price,"
                "best_ask_qty,"
                "mid_price,"
                "spread,"
                "queue_imbalance\n");
    }

    ~SnapshotWriter()
    {
        if (file_)
            fclose(file_);
    }

    // Write one TopOfBook row.
    // Prices are output in RUPEES (divided by 100) for Python readability.
    // queue_imbalance = (bid_qty - ask_qty) / (bid_qty + ask_qty)
    //   ranges -1.0 (all sell pressure) to +1.0 (all buy pressure)
    void write(const TopOfBook &tob, const char *symbol)
    {
        // Require both sides and a strictly positive spread
        if (!tob.valid())
            return;
        if (tob.best_bid_price >= tob.best_ask_price)
            return;

        double bid = tob.best_bid_price / 100.0;
        double ask = tob.best_ask_price / 100.0;
        double mid = (bid + ask) / 2.0;
        double sp = ask - bid;
        int32_t bq = tob.best_bid_qty;
        int32_t aq = tob.best_ask_qty;
        double imbal = (bq + aq > 0)
                           ? (double)(bq - aq) / (double)(bq + aq)
                           : 0.0;

        fprintf(file_,
                "%llu,%s,%.2f,%d,%.2f,%d,%.4f,%.4f,%.6f\n",
                (unsigned long long)tob.timestamp_ns,
                symbol,
                bid, bq,
                ask, aq,
                mid,
                sp,
                imbal);
        ++rows_written_;
    }

    uint64_t rows_written() const { return rows_written_; }

private:
    FILE *file_ = nullptr;
    uint64_t rows_written_ = 0;
};

// ── Sanity checker — runs after pipeline completes ────────────────────────────

struct SanityResult
{
    uint64_t total_rows = 0;
    uint64_t bid_above_ask = 0;  // hard violation — should be 0
    uint64_t zero_spread = 0;    // bid == ask (crossed book) — should be ~0
    uint64_t spread_gt_10rs = 0; // spread > ₹10 — suspicious for Nifty
    uint64_t negative_qty = 0;   // qty <= 0
    double min_spread = 1e9;
    double max_spread = 0.0;
    bool passed = false;

    void print() const
    {
        printf("\n── Sanity Check Results ─────────────────────────────────\n");
        printf("  Total rows:              %llu\n", (unsigned long long)total_rows);
        printf("  bid >= ask violations:   %llu  %s\n",
               (unsigned long long)bid_above_ask,
               bid_above_ask == 0 ? "✓" : "✗ BAD — check orderbook logic");
        printf("  Zero spread (crossed):   %llu\n", (unsigned long long)zero_spread);
        printf("  Spread > ₹10 (wide):     %llu  %s\n",
               (unsigned long long)spread_gt_10rs,
               spread_gt_10rs < total_rows * 0.01 ? "✓ (<1%% of rows)" : "! check data");
        printf("  Negative qty rows:       %llu  %s\n",
               (unsigned long long)negative_qty,
               negative_qty == 0 ? "✓" : "✗ BAD");
        printf("  Spread range:            ₹%.2f – ₹%.2f\n", min_spread, max_spread);
        printf("  Overall: %s\n", passed ? "✓ PASSED" : "✗ FAILED — check above");
        printf("────────────────────────────────────────────────────────\n");
    }
};

SanityResult run_sanity_check(const std::string &csv_path)
{
    SanityResult r{};
    std::ifstream f(csv_path);
    if (!f.is_open())
    {
        printf("Cannot open %s for sanity check\n", csv_path.c_str());
        return r;
    }

    std::string line;
    std::getline(f, line); // skip header

    while (std::getline(f, line))
    {
        if (line.empty())
            continue;
        std::stringstream ss(line);
        std::string field;
        std::vector<std::string> fields;
        while (std::getline(ss, field, ','))
            fields.push_back(field);

        if (fields.size() < 9)
            continue;
        ++r.total_rows;

        double bid = std::stod(fields[2]);
        int bid_q = std::stoi(fields[3]);
        double ask = std::stod(fields[4]);
        int ask_q = std::stoi(fields[5]);
        double spread = std::stod(fields[7]);

        if (bid >= ask)
            ++r.bid_above_ask;
        if (spread <= 0.0)
            ++r.zero_spread;
        if (spread > 10.0)
            ++r.spread_gt_10rs;
        if (bid_q <= 0 || ask_q <= 0)
            ++r.negative_qty;

        if (spread < r.min_spread && spread > 0)
            r.min_spread = spread;
        if (spread > r.max_spread)
            r.max_spread = spread;
    }

    r.passed = (r.bid_above_ask == 0) && (r.negative_qty == 0);
    return r;
}

// ── Main pipeline ─────────────────────────────────────────────────────────────

int main(int argc, char *argv[])
{
    // Defaults
    std::string input_csv = "data/nse_ticks.csv";
    std::string output_csv = "output/tob_snapshots.csv";

    if (argc >= 2)
        input_csv = argv[1];
    if (argc >= 3)
        output_csv = argv[2];

    printf("════════════════════════════════════════════════════════════════\n");
    printf("  NSE Order Book — Phase 4 Pipeline\n");
    printf("  Input:  %s\n", input_csv.c_str());
    printf("  Output: %s\n", output_csv.c_str());
    printf("════════════════════════════════════════════════════════════════\n\n");

    // ── Ensure output directory exists ────────────────────────────────────
    mkdir("output", 0755);

    // ── Initialise book and writer ────────────────────────────────────────
    PriceLadderBook::Config cfg;
    cfg.ref_price_paise = 1400'00;
    cfg.band_pct = 0.10; // wider band for full-day price movement
    PriceLadderBook book(cfg);

    SnapshotWriter writer(output_csv);
    PipelineStats stats;

    // ── Open CSV reader ───────────────────────────────────────────────────
    NSECsvReader::ColMap cols;
    // default column order matches download_nse_data.py output:
    // timestamp=0, symbol=1, order_id=2, side=3, price=4, quantity=5, msg_type=6
    NSECsvReader reader(input_csv, cols, /*has_header=*/true);

    printf("Processing events...\n");
    auto wall_start = std::chrono::steady_clock::now();

    // ── Main loop ─────────────────────────────────────────────────────────
    OrderEvent e{};
    char symbol[9] = "NIFTY50";

    while (reader.next(e))
    {
        stats.record_event(e);

        // Apply event to order book
        book.apply(e);

        // Snapshot top of book after every event
        TopOfBook tob = book.snapshot();
        tob.timestamp_ns = e.timestamp_ns;

        // Write to output CSV
        std::memcpy(symbol, e.symbol, 8);
        symbol[8] = '\0';
        writer.write(tob, symbol);

        stats.record_snapshot(tob);

        // Progress indicator every 10K events
        if (stats.total_events % 10000 == 0)
        {
            printf("  Processed %llu events, %llu snapshots written...\r",
                   (unsigned long long)stats.total_events,
                   (unsigned long long)writer.rows_written());
            fflush(stdout);
        }
    }

    auto wall_end = std::chrono::steady_clock::now();
    double elapsed_ms = std::chrono::duration<double, std::milli>(
                            wall_end - wall_start)
                            .count();

    printf("\n\nDone. Processed %llu events in %.1f ms (%.1f K events/sec)\n",
           (unsigned long long)stats.total_events,
           elapsed_ms,
           stats.total_events / elapsed_ms);

    stats.print();

    printf("\nOutput CSV: %s (%llu rows)\n",
           output_csv.c_str(),
           (unsigned long long)writer.rows_written());

    // ── Sanity check on output ────────────────────────────────────────────
    printf("\nRunning sanity checks on output CSV...\n");
    SanityResult sanity = run_sanity_check(output_csv);
    sanity.print();

    // ── Print first 5 rows for visual inspection ──────────────────────────
    printf("\nFirst 5 rows of output:\n");
    printf("%-25s %-8s %10s %6s %10s %6s %10s %8s %12s\n",
           "timestamp_ns", "symbol",
           "bid_price", "bid_q",
           "ask_price", "ask_q",
           "mid_price", "spread", "imbalance");
    printf("%s\n", std::string(95, '-').c_str());

    std::ifstream preview(output_csv);
    std::string line;
    std::getline(preview, line); // skip header
    for (int i = 0; i < 5 && std::getline(preview, line); ++i)
    {
        std::stringstream ss(line);
        std::string f;
        std::vector<std::string> fields;
        while (std::getline(ss, f, ','))
            fields.push_back(f);
        if (fields.size() >= 9)
        {
            printf("%-25s %-8s %10s %6s %10s %6s %10s %8s %12s\n",
                   fields[0].substr(0, 24).c_str(), // ts
                   fields[1].c_str(),               // symbol
                   fields[2].c_str(),               // bid
                   fields[3].c_str(),               // bid_q
                   fields[4].c_str(),               // ask
                   fields[5].c_str(),               // ask_q
                   fields[6].c_str(),               // mid
                   fields[7].c_str(),               // spread
                   fields[8].c_str());              // imbalance
        }
    }

    printf("\n");
    if (sanity.passed)
    {
        printf("✓ Phase 4 complete. Output CSV is valid.\n");
        printf("✓ Ready for Phase 5 Python analytics.\n");
    }
    else
    {
        printf("✗ Sanity check failed. Review pipeline logic before Phase 5.\n");
    }

    return sanity.passed ? 0 : 1;
}