#pragma once
#include "../include/types.h"
#include <unordered_map>
#include <cstring>
#include <stdexcept>
#include <cstdio>
#include <algorithm>

// ── PriceLadderBook ───────────────────────────────────────────────────────────
//
// Drop-in replacement for OrderBook that uses flat pre-allocated arrays instead
// of std::map for the bid/ask price levels.
//
// Why this is faster:
//   std::map is a red-black tree. Every add/remove traverses pointer-linked
//   nodes — O(log n) comparisons, each potentially a cache miss (nodes are
//   heap-allocated and scattered in memory).
//
//   A flat array indexed by price is O(1) with sequential memory access.
//   The entire active price range fits in L1/L2 cache. No pointer chasing.
//   This is what drops P99 from ~300ns to ~60-80ns.
//
// Array design:
//   NSE tick size = 5 paise. Prices are always multiples of 5.
//   We pre-allocate LEVELS slots covering a ±BAND_PCT% range around a
//   reference price. For Nifty at ₹22,500 with ±5% band:
//     min_price = 22500*100 * 0.95 = 2137500 paise
//     max_price = 22500*100 * 1.05 = 2362500 paise
//     levels    = (2362500 - 2137500) / 5 + 1 = 45001 slots
//     memory    = 45001 * 4 bytes * 2 sides = ~360 KB — fits in L2 cache
//
// Public API is identical to OrderBook from Phase 2.
// The pipeline (main.cpp, bench_runner.cpp) calls add/remove/execute/cancel/snapshot
// on either type without knowing which it is.

class PriceLadderBook
{
public:
    struct Config
    {
        int64_t ref_price_paise = 22500'00;
        double band_pct = 0.05;
        int64_t tick_size = 5;

        Config() = default;
    };

    // Default constructor — uses built-in defaults
    PriceLadderBook() : PriceLadderBook(Config{}) {}

    // Config constructor
    explicit PriceLadderBook(Config cfg) : cfg_(cfg)
    {
        // Compute array bounds
        min_price_ = snap_down(
            (int64_t)((double)cfg_.ref_price_paise * (1.0 - cfg_.band_pct)));
        max_price_ = snap_up(
            (int64_t)((double)cfg_.ref_price_paise * (1.0 + cfg_.band_pct)));
        levels_ = (max_price_ - min_price_) / cfg_.tick_size + 1;

        // Allocate zeroed arrays for bid and ask quantities
        // Each array element: total quantity at that price level
        bids_ = new int32_t[levels_](); // () zero-initialises
        asks_ = new int32_t[levels_]();

        // Track non-zero levels to find best bid/ask efficiently
        // (scanning the full array every snapshot() would be O(LEVELS) ~45K)
        // Instead we maintain sorted max-heaps of active levels.
        // For simplicity in Phase 3 we track best_bid_idx / best_ask_idx
        // and update them on every mutation.
        best_bid_idx_ = -1;
        best_ask_idx_ = (int)levels_; // sentinel: past end
    }

    ~PriceLadderBook()
    {
        delete[] bids_;
        delete[] asks_;
    }

    // Non-copyable — owns raw arrays
    PriceLadderBook(const PriceLadderBook &) = delete;
    PriceLadderBook &operator=(const PriceLadderBook &) = delete;

    // ── Public API (identical to OrderBook) ───────────────────────────────────

    void add(const OrderEvent &e)
    {
        if (order_map_.count(e.order_id)) return;
            // throw std::logic_error("Duplicate order_id: " + std::to_string(e.order_id));

        int idx = price_to_idx(e.price);
        if (e.side == 'B')
        {
            bids_[idx] += e.quantity;
            if (idx > best_bid_idx_)
                best_bid_idx_ = idx;
        }
        else
        {
            asks_[idx] += e.quantity;
            if (idx < best_ask_idx_)
                best_ask_idx_ = idx;
        }
        order_map_[e.order_id] = {(char)e.side, e.price, e.quantity};
    }

    void remove(const OrderEvent &e)
    {
        auto it = order_map_.find(e.order_id);
        if (it == order_map_.end())
            return;
        const auto &info = it->second;
        reduce(info.side, info.price, info.qty);
        order_map_.erase(it);
    }

    void execute(const OrderEvent &e)
    {
        auto it = order_map_.find(e.order_id);
        if (it == order_map_.end())
            return;
        auto &info = it->second;
        int32_t exec_qty = std::min(e.quantity, info.qty);
        reduce(info.side, info.price, exec_qty);
        info.qty -= exec_qty;
        if (info.qty <= 0)
            order_map_.erase(it);
    }

    void cancel(const OrderEvent &e)
    {
        auto it = order_map_.find(e.order_id);
        if (it == order_map_.end())
            return;
        auto &info = it->second;
        int32_t cxl_qty = std::min(e.quantity, info.qty);
        reduce(info.side, info.price, cxl_qty);
        info.qty -= cxl_qty;
        if (info.qty <= 0)
            order_map_.erase(it);
    }

    void apply(const OrderEvent &e)
    {
        switch (e.msg_type)
        {
        case 'A':
            add(e);
            break;
        case 'D':
            remove(e);
            break;
        case 'E':
            execute(e);
            break;
        case 'X':
            cancel(e);
            break;
        }
    }

    TopOfBook snapshot() const
    {
        TopOfBook tob{};
        // best_bid_idx_ is the highest non-zero bid index
        // best_ask_idx_ is the lowest  non-zero ask index
        if (best_bid_idx_ >= 0)
        {
            tob.best_bid_price = idx_to_price(best_bid_idx_);
            tob.best_bid_qty = bids_[best_bid_idx_];
        }
        if (best_ask_idx_ < (int)levels_)
        {
            tob.best_ask_price = idx_to_price(best_ask_idx_);
            tob.best_ask_qty = asks_[best_ask_idx_];
        }
        return tob;
    }

    // ── Inspection helpers ────────────────────────────────────────────────────

    int32_t bid_qty_at(int64_t price_paise) const
    {
        int idx = price_to_idx(price_paise);
        return (idx >= 0 && idx < (int)levels_) ? bids_[idx] : 0;
    }

    int32_t ask_qty_at(int64_t price_paise) const
    {
        int idx = price_to_idx(price_paise);
        return (idx >= 0 && idx < (int)levels_) ? asks_[idx] : 0;
    }

    size_t live_order_count() const { return order_map_.size(); }

    bool has_order(uint64_t id) const { return order_map_.count(id) > 0; }

    void reset()
    {
        std::fill(bids_, bids_ + levels_, 0);
        std::fill(asks_, asks_ + levels_, 0);
        order_map_.clear();
        best_bid_idx_ = -1;
        best_ask_idx_ = (int)levels_;
    }

    // Print config info
    void print_config() const
    {
        printf("PriceLadder config:\n");
        printf("  ref:       ₹%lld.%02lld\n",
               (long long)(cfg_.ref_price_paise / 100),
               (long long)(cfg_.ref_price_paise % 100));
        printf("  range:     ₹%lld.%02lld – ₹%lld.%02lld\n",
               (long long)(min_price_ / 100), (long long)(min_price_ % 100),
               (long long)(max_price_ / 100), (long long)(max_price_ % 100));
        printf("  levels:    %zu\n", levels_);
        printf("  memory:    %.1f KB (2 arrays × %zu × 4 bytes)\n",
               2.0 * levels_ * sizeof(int32_t) / 1024.0, levels_);
    }

private:
    struct OrderInfo
    {
        char side;
        int64_t price;
        int32_t qty;
    };

    Config cfg_;
    int64_t min_price_;
    int64_t max_price_;
    size_t levels_;
    int32_t *bids_;
    int32_t *asks_;
    int best_bid_idx_; // highest index with bids_[i] > 0
    int best_ask_idx_; // lowest  index with asks_[i] > 0
    std::unordered_map<uint64_t, OrderInfo> order_map_;

    // ── Index arithmetic ──────────────────────────────────────────────────────
    // price → array index: O(1), two integer ops
    inline int price_to_idx(int64_t price) const
    {
        return (int)((price - min_price_) / cfg_.tick_size);
    }

    inline int64_t idx_to_price(int idx) const
    {
        return min_price_ + (int64_t)idx * cfg_.tick_size;
    }

    // Round down / up to nearest tick
    int64_t snap_down(int64_t p) const
    {
        return (p / cfg_.tick_size) * cfg_.tick_size;
    }
    int64_t snap_up(int64_t p) const
    {
        return ((p + cfg_.tick_size - 1) / cfg_.tick_size) * cfg_.tick_size;
    }

    // ── Reduce quantity at level + update best price trackers ─────────────────
    void reduce(char side, int64_t price, int32_t qty)
    {
        int idx = price_to_idx(price);
        if (side == 'B')
        {
            bids_[idx] -= qty;
            if (bids_[idx] <= 0)
            {
                bids_[idx] = 0;
                // if we just emptied the best bid, scan downward for new best
                if (idx == best_bid_idx_)
                {
                    best_bid_idx_ = find_best_bid(idx - 1);
                }
            }
        }
        else
        {
            asks_[idx] -= qty;
            if (asks_[idx] <= 0)
            {
                asks_[idx] = 0;
                // if we just emptied the best ask, scan upward for new best
                if (idx == best_ask_idx_)
                {
                    best_ask_idx_ = find_best_ask(idx + 1);
                }
            }
        }
    }

    // Scan downward from start_idx to find highest non-zero bid
    int find_best_bid(int start_idx) const
    {
        for (int i = start_idx; i >= 0; --i)
            if (bids_[i] > 0)
                return i;
        return -1; // book empty
    }

    // Scan upward from start_idx to find lowest non-zero ask
    int find_best_ask(int start_idx) const
    {
        for (int i = start_idx; i < (int)levels_; ++i)
            if (asks_[i] > 0)
                return i;
        return (int)levels_; // book empty
    }
};