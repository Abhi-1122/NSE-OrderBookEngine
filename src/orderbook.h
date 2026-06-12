#pragma once
#include "../include/types.h"
#include <map>
#include <unordered_map>
#include <functional>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <cstdio>

// ── OrderBook ─────────────────────────────────────────────────────────────────
//
// Maintains two sides of the market for a single instrument:
//
//   bids  — buy orders, sorted descending by price (best bid = highest = begin())
//   asks  — sell orders, sorted ascending by price (best ask = lowest = begin())
//
// Each side maps:  price (paise) → total quantity at that level
//
// order_map provides O(1) lookup by order_id so that Delete and Execute events
// don't need to scan the book. Without it, removing an order would require
// knowing its price level first — which the event doesn't always carry.
//
// This is the std::map version (Phase 2).
// Phase 3 replaces the price-level structure with a flat array while keeping
// this exact public API unchanged.

class OrderBook {
public:

    // ── Public API ────────────────────────────────────────────────────────────
    // All four handlers accept a full OrderEvent.
    // snapshot() returns the current best bid/ask state.

    void add(const OrderEvent& e);
    void remove(const OrderEvent& e);   // msg_type 'D'
    void execute(const OrderEvent& e);  // msg_type 'E' — partial or full fill
    void cancel(const OrderEvent& e);   // msg_type 'X' — partial quantity cancel

    // Dispatch by msg_type — convenience for pipeline use
    void apply(const OrderEvent& e) {
        switch (e.msg_type) {
            case 'A': add(e);     break;
            case 'D': remove(e);  break;
            case 'E': execute(e); break;
            case 'X': cancel(e);  break;
            default:
                throw std::invalid_argument("Unknown msg_type");
        }
    }

    TopOfBook snapshot() const;

    // ── Inspection helpers (for unit tests and debugging) ─────────────────────

    // Total quantity at a specific price on the bid side
    int32_t bid_qty_at(int64_t price_paise) const {
        auto it = bids_.find(price_paise);
        return (it != bids_.end()) ? it->second : 0;
    }

    // Total quantity at a specific price on the ask side
    int32_t ask_qty_at(int64_t price_paise) const {
        auto it = asks_.find(price_paise);
        return (it != asks_.end()) ? it->second : 0;
    }

    // Number of distinct price levels on each side
    size_t bid_levels() const { return bids_.size(); }
    size_t ask_levels() const { return asks_.size(); }

    // Total number of live orders tracked in order_map
    size_t live_order_count() const { return order_map_.size(); }

    // Check if an order_id exists in the book
    bool has_order(uint64_t order_id) const {
        return order_map_.count(order_id) > 0;
    }

    // Dump book state to stdout (for manual inspection)
    void print(int levels = 5) const;

    // Clear all state
    void reset() {
        bids_.clear();
        asks_.clear();
        order_map_.clear();
    }

private:

    // ── Internal record kept in order_map ─────────────────────────────────────
    // Stores everything needed to locate and modify an order later.
    struct OrderInfo {
        char    side;       // 'B' or 'S'
        int64_t price;      // paise — index into bids_ or asks_
        int32_t qty;        // remaining quantity
    };

    // ── Price-level maps ──────────────────────────────────────────────────────
    // bids_ uses std::greater so begin() is the highest (best) bid.
    // asks_ uses default less<> so begin() is the lowest (best) ask.
    std::map<int64_t, int32_t, std::greater<int64_t>> bids_;
    std::map<int64_t, int32_t>                         asks_;

    // ── Order-level index ─────────────────────────────────────────────────────
    // order_id → OrderInfo  (O(1) average lookup)
    std::unordered_map<uint64_t, OrderInfo> order_map_;

    // ── Private helpers ───────────────────────────────────────────────────────

    // Returns reference to the correct side map for a given side char
    // Note: can't return reference to either of two different types directly,
    // so we use template helpers below.

    void reduce_level(char side, int64_t price, int32_t qty);
    void remove_level_if_empty(char side, int64_t price);
};

// ── Implementation ────────────────────────────────────────────────────────────

inline void OrderBook::add(const OrderEvent& e) {
    // Reject duplicates — same order_id added twice is a feed error
    if (order_map_.count(e.order_id)) {
        // In production: log and skip. In tests: throw to catch bad data.
        throw std::logic_error("Duplicate order_id in Add: " +
                               std::to_string(e.order_id));
    }

    // Add quantity to the price level
    if (e.side == 'B') {
        bids_[e.price] += e.quantity;
    } else {
        asks_[e.price] += e.quantity;
    }

    // Record in order_map for O(1) future delete/execute/cancel
    order_map_[e.order_id] = OrderInfo{(char)e.side, e.price, e.quantity};
}

inline void OrderBook::remove(const OrderEvent& e) {
    // 'D' Delete — full removal of an order
    auto it = order_map_.find(e.order_id);
    if (it == order_map_.end()) return; // stale or already removed — skip silently

    const OrderInfo& info = it->second;

    // Subtract from price level
    reduce_level(info.side, info.price, info.qty);

    // Remove from order index
    order_map_.erase(it);
}

inline void OrderBook::execute(const OrderEvent& e) {
    // 'E' Execute — a trade occurred. e.quantity is the executed amount.
    // The order may be partially filled (residual remains) or fully filled.
    auto it = order_map_.find(e.order_id);
    if (it == order_map_.end()) return;

    OrderInfo& info = it->second;

    // Clamp: can't execute more than what's on the book
    int32_t exec_qty = std::min(e.quantity, info.qty);

    // Reduce the price level by executed quantity
    reduce_level(info.side, info.price, exec_qty);

    // Update residual quantity in order_map
    info.qty -= exec_qty;

    // If fully filled, remove from order index
    if (info.qty <= 0) {
        order_map_.erase(it);
    }
}

inline void OrderBook::cancel(const OrderEvent& e) {
    // 'X' Cancel — partial quantity removed from an order
    // Structurally identical to execute but semantically different:
    // no trade occurred, a portion of the order was withdrawn.
    auto it = order_map_.find(e.order_id);
    if (it == order_map_.end()) return;

    OrderInfo& info = it->second;
    int32_t cancel_qty = std::min(e.quantity, info.qty);

    reduce_level(info.side, info.price, cancel_qty);
    info.qty -= cancel_qty;

    if (info.qty <= 0) {
        order_map_.erase(it);
    }
}

inline TopOfBook OrderBook::snapshot() const {
    TopOfBook tob{};
    tob.timestamp_ns = 0; // caller stamps this with rdtsc in Phase 3

    if (!bids_.empty()) {
        auto it = bids_.begin(); // best bid = highest price
        tob.best_bid_price = it->first;
        tob.best_bid_qty   = it->second;
    }
    if (!asks_.empty()) {
        auto it = asks_.begin(); // best ask = lowest price
        tob.best_ask_price = it->first;
        tob.best_ask_qty   = it->second;
    }
    return tob;
}

inline void OrderBook::reduce_level(char side, int64_t price, int32_t qty) {
    if (side == 'B') {
        auto it = bids_.find(price);
        if (it == bids_.end()) return;
        it->second -= qty;
        if (it->second <= 0) bids_.erase(it);
    } else {
        auto it = asks_.find(price);
        if (it == asks_.end()) return;
        it->second -= qty;
        if (it->second <= 0) asks_.erase(it);
    }
}

inline void OrderBook::print(int levels) const {
    printf("\n┌─── Order Book ────────────────────────────────┐\n");

    // Print top N ask levels in reverse (worst ask first so best ask is closest to spread)
    std::vector<std::pair<int64_t,int32_t>> ask_vec(asks_.begin(), asks_.end());
    int ask_start = std::max(0, (int)ask_vec.size() - levels);
    for (int i = (int)ask_vec.size() - 1; i >= ask_start; --i) {
        printf("│  ASK  ₹%6lld.%02lld   qty: %6d               │\n",
               (long long)(ask_vec[i].first / 100),
               (long long)(ask_vec[i].first % 100),
               ask_vec[i].second);
    }

    // Spread line
    if (!bids_.empty() && !asks_.empty()) {
        int64_t spread = asks_.begin()->first - bids_.begin()->first;
        printf("│  ── spread: ₹%lld.%02lld ──────────────────────── │\n",
               (long long)(spread / 100), (long long)(spread % 100));
    } else {
        printf("│  ── (one side empty) ─────────────────────── │\n");
    }

    // Print top N bid levels
    auto bit = bids_.begin();
    for (int i = 0; i < levels && bit != bids_.end(); ++i, ++bit) {
        printf("│  BID  ₹%6lld.%02lld   qty: %6d               │\n",
               (long long)(bit->first / 100),
               (long long)(bit->first % 100),
               bit->second);
    }

    printf("└───────────────────────────────────────────────┘\n");
    printf("  Live orders in index: %zu  |  Bid levels: %zu  |  Ask levels: %zu\n\n",
           order_map_.size(), bids_.size(), asks_.size());
}