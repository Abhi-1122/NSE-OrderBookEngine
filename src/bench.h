#pragma once
#include <cstdint>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <string>
#include <numeric>

#ifdef _MSC_VER
#include <intrin.h>
#define RDTSC() __rdtsc()
#else
#include <x86intrin.h>
#define RDTSC() __rdtsc()
#endif

// ── Bench ─────────────────────────────────────────────────────────────────────
// Wraps rdtsc-based nanosecond latency measurement.
//
// Why rdtsc and not clock_gettime:
//   clock_gettime(CLOCK_MONOTONIC) involves a vDSO call — ~20-30ns overhead
//   on Linux. At sub-100ns measurement scale that overhead dominates the signal.
//   rdtsc reads the CPU hardware time-stamp counter directly in a single
//   instruction: no syscall, no kernel transition, ~1-5 cycle overhead.
//
// Usage:
//   Bench b("my operation");
//   for (int i = 0; i < N; ++i) {
//       b.start();
//       fn();
//       b.stop();
//   }
//   b.report();

class Bench
{
public:
    explicit Bench(const std::string &name, double cpu_ghz = 0.0)
        : name_(name), cpu_ghz_(cpu_ghz)
    {
        if (cpu_ghz_ <= 0.0)
            cpu_ghz_ = detect_cpu_ghz();
        samples_.reserve(1 << 20); // pre-allocate 1M slots — no realloc during hot loop
    }

    // Call immediately before the operation under test
    inline void start()
    {
        // Serialise instruction stream before reading TSC.
        // Without this, out-of-order execution can reorder rdtsc before the
        // work we're measuring — giving an artifically low reading.
        // lfence is cheaper than cpuid and sufficient for a load fence.
        _mm_lfence();
        t0_ = RDTSC();
    }

    // Call immediately after the operation under test
    inline void stop()
    {
        uint64_t t1 = RDTSC();
        _mm_lfence();
        samples_.push_back(t1 - t0_);
    }

    // Convenience: record a pre-measured cycle delta directly
    inline void record(uint64_t cycles)
    {
        samples_.push_back(cycles);
    }

    void clear() { samples_.clear(); }

    // Print full latency report to stdout
    // Single helper that sorts + trims once and returns the trimmed vector
    std::vector<uint64_t> trimmed_sorted() const
    {
        std::vector<uint64_t> s = samples_;
        std::sort(s.begin(), s.end());
        size_t keep = (size_t)(s.size() * 0.995); // discard top 0.5% as OS noise
        s.resize(keep);
        return s;
    }

    void report() const
    {
        if (samples_.empty())
        {
            printf("No samples recorded.\n");
            return;
        }
        auto s = trimmed_sorted();
        size_t n = s.size();

        printf("\n┌─── Benchmark: %-34s ───┐\n", name_.c_str());
        printf("│  Samples:     %-10zu (top 0.5%% OS noise trimmed)       │\n", n);
        printf("│  CPU speed:   %-6.2f GHz                               │\n", cpu_ghz_);
        printf("│                                                         │\n");
        printf("│  Latency (nanoseconds):                                 │\n");
        printf("│    Min:    %8.1f ns                                 │\n", cycles_to_ns(s[0]));
        printf("│    P50:    %8.1f ns                                 │\n", cycles_to_ns(percentile(s, 50.0)));
        printf("│    P90:    %8.1f ns                                 │\n", cycles_to_ns(percentile(s, 90.0)));
        printf("│    P99:    %8.1f ns  ← resume number               │\n", cycles_to_ns(percentile(s, 99.0)));
        printf("│    P99.9:  %8.1f ns                                 │\n", cycles_to_ns(percentile(s, 99.9)));
        printf("│                                                         │\n");
        printf("│  Throughput: %.1f M ops/sec                            │\n",
               cpu_ghz_ * 1000.0 / (double)mean(s));
        printf("└─────────────────────────────────────────────────────────┘\n");
    }

    double p50_ns() const
    {
        auto s = trimmed_sorted();
        return s.empty() ? 0.0 : cycles_to_ns(percentile(s, 50.0));
    }
    double p99_ns() const
    {
        auto s = trimmed_sorted();
        return s.empty() ? 0.0 : cycles_to_ns(percentile(s, 99.0));
    }
    double p999_ns() const
    {
        auto s = trimmed_sorted();
        return s.empty() ? 0.0 : cycles_to_ns(percentile(s, 99.9));
    }

    const std::string &name() const { return name_; }

private:
    std::string name_;
    double cpu_ghz_;
    uint64_t t0_{0};
    std::vector<uint64_t> samples_;

    inline double cycles_to_ns(uint64_t cycles) const
    {
        return (double)cycles / cpu_ghz_;
    }

    static uint64_t percentile(const std::vector<uint64_t> &sorted, double p)
    {
        size_t idx = (size_t)(p / 100.0 * (double)sorted.size());
        if (idx >= sorted.size())
            idx = sorted.size() - 1;
        return sorted[idx];
    }

    static double mean(const std::vector<uint64_t> &v)
    {
        return (double)std::accumulate(v.begin(), v.end(), 0ULL) / (double)v.size();
    }

    // Auto-detect CPU frequency from /proc/cpuinfo (Linux only)
    // Falls back to 3.0 GHz if unavailable
    static double detect_cpu_ghz()
    {
        FILE *f = fopen("/proc/cpuinfo", "r");
        if (!f)
            return 3.0;
        char line[256];
        while (fgets(line, sizeof(line), f))
        {
            double mhz = 0.0;
            if (sscanf(line, "cpu MHz : %lf", &mhz) == 1 && mhz > 100.0)
            {
                fclose(f);
                return mhz / 1000.0;
            }
        }
        fclose(f);
        return 3.0;
    }
};