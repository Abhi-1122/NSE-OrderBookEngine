#include <cstdio>
#include <cassert>
#include <cstring>
#include <vector>
#include <numeric>
#include <algorithm>
#include <random>

#include "../include/types.h"
#include "orderbook.h"

// ── Test harness ──────────────────────────────────────────────────────────────

static int tests_run    = 0;
static int tests_passed = 0;

#define CHECK(cond, msg)                                               \
    do {                                                               \
        ++tests_run;                                                   \
        if (cond) {                                                    \
            ++tests_passed;                                            \
        } else {                                                       \
            printf("  ✗ FAIL  line %d: %s\n", __LINE__, msg);         \
        }                                                              \
    } while(0)

#define SECTION(name) printf("\n── %s ──\n", name)

// ── Event factory ─────────────────────────────────────────────────────────────
// Builds OrderEvent structs cleanly for tests — avoids repetitive field setup.

static uint64_t g_ts = 1000000000ULL;   // simulated timestamp
static uint64_t g_id = 1;               // auto-incrementing order id

static OrderEvent make_add(char side, int64_t price_paise, int32_t qty,
                           uint64_t order_id = 0) {
    OrderEvent e{};
    e.msg_type    = 'A';
    e.timestamp_ns = g_ts += 1000;
    e.order_id    = (order_id > 0) ? order_id : g_id++;
    e.side        = (uint8_t)side;
    e.price       = price_paise;
    e.quantity    = qty;
    std::memcpy(e.symbol, "TEST\0\0\0\0", 8);
    return e;
}

static OrderEvent make_event(char msg_type, uint64_t order_id,
                              char side, int64_t price_paise, int32_t qty) {
    OrderEvent e{};
    e.msg_type     = (uint8_t)msg_type;
    e.timestamp_ns = g_ts += 1000;
    e.order_id     = order_id;
    e.side         = (uint8_t)side;
    e.price        = price_paise;
    e.quantity     = qty;
    std::memcpy(e.symbol, "TEST\0\0\0\0", 8);
    return e;
}

// ── Tests ─────────────────────────────────────────────────────────────────────

// ── Test 1: Add 100 buy orders, verify best bid ───────────────────────────────
void test_add_100_buys() {
    SECTION("Test 1: Add 100 buy orders — best bid correctness");

    OrderBook book;
    g_id = 1;

    // Add 100 buy orders at prices from ₹22,400.00 to ₹22,499.95 (5p steps)
    // Best bid should be the highest price: ₹22,499.95 = 2249995 paise
    int64_t highest_price = 0;
    for (int i = 0; i < 100; ++i) {
        int64_t price = 2240000 + (int64_t)i * 5; // 22400.00, 22400.05, ..., 22449.95
        OrderEvent e  = make_add('B', price, 50 + i);
        book.add(e);
        if (price > highest_price) highest_price = price;
    }

    TopOfBook tob = book.snapshot();

    CHECK(tob.best_bid_price == highest_price,
          "best bid should be highest added buy price");
    CHECK(tob.best_bid_qty == 149,
          "qty at best bid should be 50+99=149 (last order)");
    CHECK(book.bid_levels() == 100,
          "should have 100 distinct bid price levels");
    CHECK(book.live_order_count() == 100,
          "order_map should track all 100 orders");
    CHECK(tob.best_ask_price == 0,
          "no asks added — best ask should be 0");

    printf("  Best bid: ₹%lld.%02lld  qty: %d\n",
           (long long)(tob.best_bid_price/100), (long long)(tob.best_bid_price%100),
           tob.best_bid_qty);
}

// ── Test 2: Add 100 sell orders, verify best ask ──────────────────────────────
void test_add_100_sells() {
    SECTION("Test 2: Add 100 sell orders — best ask correctness");

    OrderBook book;
    g_id = 1000;

    // Prices from ₹22,500.00 to ₹22,549.95 — best ask = lowest = ₹22,500.00
    int64_t lowest_price = INT64_MAX;
    for (int i = 0; i < 100; ++i) {
        int64_t price = 2250000 + (int64_t)i * 5;
        OrderEvent e  = make_add('S', price, 30 + i);
        book.add(e);
        if (price < lowest_price) lowest_price = price;
    }

    TopOfBook tob = book.snapshot();

    CHECK(tob.best_ask_price == lowest_price,
          "best ask should be lowest added sell price");
    CHECK(tob.best_ask_qty == 30,
          "qty at best ask should be 30 (first order, i=0)");
    CHECK(book.ask_levels() == 100,
          "should have 100 distinct ask price levels");
    CHECK(book.live_order_count() == 100,
          "order_map should track all 100 sell orders");
    CHECK(tob.best_bid_price == 0,
          "no bids added — best bid should be 0");

    printf("  Best ask: ₹%lld.%02lld  qty: %d\n",
           (long long)(tob.best_ask_price/100), (long long)(tob.best_ask_price%100),
           tob.best_ask_qty);
}

// ── Test 3: Multiple orders at same price level ───────────────────────────────
void test_aggregation_at_level() {
    SECTION("Test 3: Multiple orders at same price — quantity aggregation");

    OrderBook book;
    g_id = 2000;

    // Five buy orders all at ₹22,450.00 = 2245000 paise, qty 100 each
    for (int i = 0; i < 5; ++i) {
        book.add(make_add('B', 2245000, 100));
    }

    CHECK(book.bid_qty_at(2245000) == 500,
          "5 orders × qty 100 should aggregate to 500 at that level");
    CHECK(book.bid_levels() == 1,
          "all at same price — should be 1 level only");
    CHECK(book.live_order_count() == 5,
          "order_map should still track 5 individual orders");

    printf("  qty at ₹22450.00: %d\n", book.bid_qty_at(2245000));
}

// ── Test 4: Delete 30 orders, verify quantities ───────────────────────────────
void test_delete_30_orders() {
    SECTION("Test 4: Delete 30 orders — quantity correctness");

    OrderBook book;
    g_id = 3000;

    // Add 100 buy orders, track their ids
    std::vector<std::pair<uint64_t, int64_t>> added; // {order_id, price}
    for (int i = 0; i < 100; ++i) {
        int64_t price = 2240000 + (int64_t)(i % 10) * 5; // 10 distinct levels, 10 orders each
        uint64_t id   = g_id;
        book.add(make_add('B', price, 50));
        added.push_back({id, price});
    }

    CHECK(book.live_order_count() == 100, "should have 100 live orders before delete");
    CHECK(book.bid_levels() == 10,        "should have 10 price levels before delete");

    // Delete 30 orders spread across different price levels
    int64_t qty_before_level0 = book.bid_qty_at(2240000); // price level i%10==0
    int deleted_from_level0   = 0;

    for (int i = 0; i < 30; ++i) {
        auto [oid, price] = added[i];
        book.remove(make_event('D', oid, 'B', price, 50));
        if (price == 2240000) ++deleted_from_level0;
    }

    // orders 0,10,20 had price 2240000 (i%10==0) — all 3 deleted
    int64_t expected_qty = qty_before_level0 - deleted_from_level0 * 50;

    CHECK(book.live_order_count() == 70, "70 orders should remain after 30 deletes");
    CHECK(book.bid_qty_at(2240000) == expected_qty,
          "qty at level 0 should be reduced by deleted orders");
    CHECK(!book.has_order(added[0].first),  "deleted order 0 should not be in order_map");
    CHECK(!book.has_order(added[15].first), "deleted order 15 should not be in order_map");
    CHECK(book.has_order(added[30].first),  "non-deleted order 30 should still be in order_map");

    printf("  Orders remaining: %zu  |  Price levels: %zu\n",
           book.live_order_count(), book.bid_levels());
}

// ── Test 5: Execute 20 orders partially, verify residuals ────────────────────
void test_partial_execute() {
    SECTION("Test 5: Partial execute — residual quantity correctness");

    OrderBook book;
    g_id = 4000;

    // Add 20 sell orders, qty 100 each, at ₹22,500.05 to ₹22,500.15
    std::vector<uint64_t> order_ids;
    for (int i = 0; i < 20; ++i) {
        int64_t price = 2250005 + (int64_t)i * 5;
        order_ids.push_back(g_id);
        book.add(make_add('S', price, 100));
    }

    CHECK(book.live_order_count() == 20, "20 orders before execute");

    // Execute 30 units (partial fill) on each of the 20 orders
    for (int i = 0; i < 20; ++i) {
        int64_t price = 2250005 + (int64_t)i * 5;
        book.execute(make_event('E', order_ids[i], 'S', price, 30));
    }

    // Each order had 100, executed 30 → 70 residual
    // Level qty for each: was 100, now 70
    CHECK(book.live_order_count() == 20,
          "partially filled orders stay in order_map");

    bool all_residuals_correct = true;
    for (int i = 0; i < 20; ++i) {
        int64_t price = 2250005 + (int64_t)i * 5;
        if (book.ask_qty_at(price) != 70) {
            all_residuals_correct = false;
            break;
        }
    }
    CHECK(all_residuals_correct,
          "all 20 levels should have residual qty of 70 after partial execute");

    printf("  All 20 partial executes: 100 - 30 = 70 residual ✓\n");
}

// ── Test 6: Full execute removes level ───────────────────────────────────────
void test_full_execute_removes_level() {
    SECTION("Test 6: Full execute — level disappears at qty 0");

    OrderBook book;
    g_id = 5000;

    // Single sell order at best ask
    uint64_t oid = g_id;
    book.add(make_add('S', 2250000, 75));

    CHECK(book.ask_levels() == 1,       "one ask level before execute");
    CHECK(book.ask_qty_at(2250000) == 75, "qty 75 before execute");

    // Execute exactly 75 — full fill
    book.execute(make_event('E', oid, 'S', 2250000, 75));

    CHECK(book.ask_levels() == 0,         "ask level should disappear after full execute");
    CHECK(book.ask_qty_at(2250000) == 0,  "qty at that level should be 0");
    CHECK(!book.has_order(oid),           "fully executed order removed from order_map");
    CHECK(book.live_order_count() == 0,   "no live orders remain");

    TopOfBook tob = book.snapshot();
    CHECK(tob.best_ask_price == 0, "best ask should be 0 when book is empty");

    printf("  Full execute: level removed, order_map empty ✓\n");
}

// ── Test 7: Cancel (partial quantity reduction) ───────────────────────────────
void test_cancel() {
    SECTION("Test 7: Cancel — partial quantity reduction");

    OrderBook book;
    g_id = 6000;

    uint64_t oid = g_id;
    book.add(make_add('B', 2245000, 200));

    CHECK(book.bid_qty_at(2245000) == 200, "qty 200 before cancel");

    // Cancel 80 units
    book.cancel(make_event('X', oid, 'B', 2245000, 80));

    CHECK(book.bid_qty_at(2245000) == 120, "qty should be 120 after cancelling 80");
    CHECK(book.has_order(oid),             "order still live after partial cancel");

    // Cancel remaining 120 — full cancel
    book.cancel(make_event('X', oid, 'B', 2245000, 120));

    CHECK(book.bid_qty_at(2245000) == 0, "qty 0 after full cancel");
    CHECK(book.bid_levels() == 0,        "level removed after qty hits 0");
    CHECK(!book.has_order(oid),          "order removed from order_map after full cancel");

    printf("  Partial cancel (80) + full cancel (120): level removed ✓\n");
}

// ── Test 8: Best bid/ask updates correctly after deletes ──────────────────────
void test_best_price_updates() {
    SECTION("Test 8: Best bid/ask updates correctly as levels empty");

    OrderBook book;
    g_id = 7000;

    // Three bid levels: 100, 200, 300 paise (3 orders each, qty 10)
    std::vector<uint64_t> ids_300, ids_200, ids_100;
    for (int i = 0; i < 3; ++i) {
        ids_300.push_back(g_id); book.add(make_add('B', 300, 10));
        ids_200.push_back(g_id); book.add(make_add('B', 200, 10));
        ids_100.push_back(g_id); book.add(make_add('B', 100, 10));
    }

    CHECK(book.snapshot().best_bid_price == 300, "initial best bid should be 300");

    // Delete all three orders at level 300
    for (auto id : ids_300)
        book.remove(make_event('D', id, 'B', 300, 10));

    CHECK(book.snapshot().best_bid_price == 200,
          "best bid should fall to 200 after level 300 cleared");
    CHECK(book.bid_levels() == 2, "should have 2 levels after clearing top");

    // Delete all at level 200
    for (auto id : ids_200)
        book.remove(make_event('D', id, 'B', 200, 10));

    CHECK(book.snapshot().best_bid_price == 100,
          "best bid should fall to 100 after level 200 cleared");

    printf("  Best bid progression: 300 → 200 → 100 ✓\n");
}

// ── Test 9: Spread sanity (bid always below ask) ──────────────────────────────
void test_spread_sanity() {
    SECTION("Test 9: Spread sanity — bid always < ask");

    OrderBook book;
    g_id = 8000;

    // Add mixed bids and asks
    for (int i = 0; i < 50; ++i) {
        int64_t bid_price = 2249000 - (int64_t)i * 5; // descending bids
        int64_t ask_price = 2250000 + (int64_t)i * 5; // ascending asks
        book.add(make_add('B', bid_price, 25));
        book.add(make_add('S', ask_price, 25));
    }

    TopOfBook tob = book.snapshot();

    CHECK(tob.best_bid_price < tob.best_ask_price,
          "best bid must always be strictly less than best ask");
    CHECK(tob.spread_paise() > 0, "spread must be positive");
    CHECK(tob.spread_paise() == 1000,
          "spread should be 2250000 - 2249000 = 1000 paise = ₹10.00");

    printf("  Best bid: ₹%lld.%02lld  Best ask: ₹%lld.%02lld  Spread: ₹%lld.%02lld\n",
           (long long)(tob.best_bid_price/100), (long long)(tob.best_bid_price%100),
           (long long)(tob.best_ask_price/100), (long long)(tob.best_ask_price%100),
           (long long)(tob.spread_paise()/100),  (long long)(tob.spread_paise()%100));
}

// ── Test 10: Stale event handling — delete unknown order_id ──────────────────
void test_stale_event() {
    SECTION("Test 10: Stale event handling — unknown order_id is ignored");

    OrderBook book;
    g_id = 9000;

    book.add(make_add('B', 2245000, 100));

    // Delete an order_id that was never added — should not throw or corrupt state
    book.remove(make_event('D', 99999999ULL, 'B', 2245000, 100));

    CHECK(book.bid_qty_at(2245000) == 100,
          "qty unchanged after stale delete");
    CHECK(book.live_order_count() == 1,
          "order_map unchanged after stale delete");

    printf("  Stale delete silently ignored — book unchanged ✓\n");
}

// ── Test 11: Mixed sequence — full integration test ───────────────────────────
void test_mixed_sequence() {
    SECTION("Test 11: Full mixed-event sequence — manual verification");

    OrderBook book;
    g_id = 10000;

    //  1. Add 5 buys at ₹224.95, qty 100 each
    std::vector<uint64_t> buy_ids;
    for (int i = 0; i < 5; ++i) {
        buy_ids.push_back(g_id);
        book.add(make_add('B', 22495, 100));
    }

    //  2. Add 5 sells at ₹225.00, qty 80 each
    std::vector<uint64_t> sell_ids;
    for (int i = 0; i < 5; ++i) {
        sell_ids.push_back(g_id);
        book.add(make_add('S', 22500, 80));
    }

    //  Verify initial state
    CHECK(book.bid_qty_at(22495) == 500, "bid qty: 5×100 = 500");
    CHECK(book.ask_qty_at(22500) == 400, "ask qty: 5×80  = 400");

    //  3. Execute 2 sell orders fully (160 total executed)
    book.execute(make_event('E', sell_ids[0], 'S', 22500, 80));
    book.execute(make_event('E', sell_ids[1], 'S', 22500, 80));

    CHECK(book.ask_qty_at(22500) == 240, "ask qty after 2 full executes: 400-160=240");
    CHECK(book.live_order_count() == 8,  "8 live orders: 5 buys + 3 remaining sells");

    //  4. Cancel 1 buy order partially (50 of 100)
    book.cancel(make_event('X', buy_ids[0], 'B', 22495, 50));

    CHECK(book.bid_qty_at(22495) == 450, "bid qty after cancel 50: 500-50=450");
    CHECK(book.has_order(buy_ids[0]),    "partially cancelled order still live");

    //  5. Delete 2 buy orders fully
    book.remove(make_event('D', buy_ids[1], 'B', 22495, 100));
    book.remove(make_event('D', buy_ids[2], 'B', 22495, 100));

    CHECK(book.bid_qty_at(22495) == 250,  "bid qty after 2 deletes: 450-200=250");
    CHECK(book.live_order_count() == 6,   "6 live orders after 2 deletes");

    //  6. Snapshot
    TopOfBook tob = book.snapshot();
    CHECK(tob.best_bid_price == 22495, "best bid still ₹224.95");
    CHECK(tob.best_ask_price == 22500, "best ask still ₹225.00");
    CHECK(tob.spread_paise()  == 5,    "spread = 5 paise = ₹0.05 (1 tick)");

    book.print(3); // visual inspection
    printf("  Mixed sequence: all checks passed ✓\n");
}

// ── Test 12: Overflow — execute more than available is clamped ────────────────
void test_execute_clamp() {
    SECTION("Test 12: Execute qty > order qty is clamped, no underflow");

    OrderBook book;
    g_id = 11000;

    uint64_t oid = g_id;
    book.add(make_add('S', 2250000, 50));

    // Try to execute 200 when only 50 exists
    book.execute(make_event('E', oid, 'S', 2250000, 200));

    CHECK(book.ask_qty_at(2250000) == 0, "level should be 0 after clamped full execute");
    CHECK(!book.has_order(oid),          "order should be removed after clamped execute");
    CHECK(book.ask_levels() == 0,        "no ask levels should remain");

    printf("  Over-execute clamped to available qty — no underflow ✓\n");
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main() {
    printf("════════════════════════════════════════════════════\n");
    printf("  NSE Order Book — Phase 2 Unit Tests\n");
    printf("════════════════════════════════════════════════════\n");

    test_add_100_buys();
    test_add_100_sells();
    test_aggregation_at_level();
    test_delete_30_orders();
    test_partial_execute();
    test_full_execute_removes_level();
    test_cancel();
    test_best_price_updates();
    test_spread_sanity();
    test_stale_event();
    test_mixed_sequence();
    test_execute_clamp();

    printf("\n════════════════════════════════════════════════════\n");
    printf("  Results: %d / %d tests passed\n", tests_passed, tests_run);
    if (tests_passed == tests_run) {
        printf("  ✓ ALL TESTS PASSED — Phase 2 complete\n");
    } else {
        printf("  ✗ %d TESTS FAILED\n", tests_run - tests_passed);
    }
    printf("════════════════════════════════════════════════════\n");

    return (tests_passed == tests_run) ? 0 : 1;
}