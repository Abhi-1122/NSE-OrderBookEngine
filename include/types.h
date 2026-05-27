#pragma once
#include <cstdint>
#include <cstring>
#include <string>

// ── OrderEvent ────────────────────────────────────────────────────────────────
// Single canonical message format. Every data source (CSV, WebSocket, synthetic)
// converts into this before touching the order book.

#pragma pack(push, 1)
struct OrderEvent {
    uint8_t  msg_type;        // 'A' add | 'D' delete | 'E' execute | 'X' cancel
    uint64_t timestamp_ns;    // nanoseconds since Unix epoch
    uint64_t order_id;        // unique order reference number
    uint8_t  side;            // 'B' buy | 'S' sell
    int64_t  price;           // price in PAISE — never a float
    int32_t  quantity;        // number of shares/units
    char     symbol[8];       // null-padded NSE symbol e.g. "NIFTY50\0"
};
#pragma pack(pop)


// ── TopOfBook ─────────────────────────────────────────────────────────────────
// Snapshot of best bid and ask after each order book update.

struct TopOfBook {
    int64_t  best_bid_price;  // paise
    int64_t  best_ask_price;  // paise
    int32_t  best_bid_qty;
    int32_t  best_ask_qty;
    uint64_t timestamp_ns;
    char     symbol[8];

    // helper: are both sides present?
    bool valid() const {
        return best_bid_price > 0 && best_ask_price > 0;
    }

    // helper: spread in paise
    int64_t spread_paise() const {
        return best_ask_price - best_bid_price;
    }

    // helper: mid price in paise (integer, truncated)
    int64_t mid_paise() const {
        return (best_bid_price + best_ask_price) / 2;
    }
};

// ── Utility ───────────────────────────────────────────────────────────────────

// Convert rupee string "245.60" → paise int64_t 24560
// Handles up to 2 decimal places. No floating point used.
inline int64_t rupees_to_paise(const std::string& s) {
    // find decimal point
    size_t dot = s.find('.');
    int64_t rupee_part = 0;
    int64_t paise_part = 0;

    if (dot == std::string::npos) {
        rupee_part = std::stoll(s);
        paise_part = 0;
    } else {
        rupee_part = std::stoll(s.substr(0, dot));
        std::string frac = s.substr(dot + 1);
        // normalise to exactly 2 digits
        if (frac.size() == 1) frac += "0";
        if (frac.size() > 2)  frac = frac.substr(0, 2);
        paise_part = std::stoll(frac);
    }
    return rupee_part * 100 + paise_part;
}

// Round paise to nearest 5 (NSE tick = ₹0.05 = 5 paise)
inline int64_t snap_to_tick(int64_t paise) {
    return ((paise + 2) / 5) * 5;
}

// Print an OrderEvent to stdout (for debugging)
inline void print_event(const OrderEvent& e) {
    char sym[9] = {};
    std::memcpy(sym, e.symbol, 8);
    printf("[%c] ts=%llu id=%llu side=%c price=₹%lld.%02lld qty=%d sym=%s\n",
           e.msg_type,
           (unsigned long long)e.timestamp_ns,
           (unsigned long long)e.order_id,
           e.side,
           (long long)(e.price / 100),
           (long long)(e.price % 100),
           e.quantity,
           sym);
}