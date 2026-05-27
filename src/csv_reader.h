#pragma once
#include "../include/types.h"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <stdexcept>
#include <cstring>

// ── NSECsvReader ──────────────────────────────────────────────────────────────
// Parses NSE historical intraday tick CSV into OrderEvent structs.
//
// Expected CSV format (NSE Bhavcopy / Kite historical export):
//   timestamp,symbol,order_id,side,price,quantity,msg_type
//   2024-01-15 09:15:00.123456789,NIFTY50,100001,B,22450.25,50,A
//
// If your CSV has different column names, adjust col_* indices below.
// Price column must be in rupees (string like "22450.25") — converted to paise here.
//
// Same output type as SyntheticGenerator: OrderEvent
// The order book never knows which source it came from.

class NSECsvReader {
public:
    struct ColMap {
        ColMap() = default;
        int timestamp = 0;
        int symbol    = 1;
        int order_id  = 2;
        int side      = 3;
        int price     = 4;
        int quantity  = 5;
        int msg_type  = 6;
    };

    explicit NSECsvReader(const std::string& filepath, bool has_header = true)
        : NSECsvReader(filepath, ColMap{}, has_header) {}

    explicit NSECsvReader(const std::string& filepath, ColMap cols, bool has_header = true)
        : cols_(cols)
    {
        file_.open(filepath);
        if (!file_.is_open())
            throw std::runtime_error("Cannot open CSV: " + filepath);

        if (has_header) {
            std::string header;
            std::getline(file_, header); // skip header row
        }
    }

    // Returns false when file is exhausted
    bool next(OrderEvent& out) {
        std::string line;
        while (std::getline(file_, line)) {
            if (line.empty() || line[0] == '#') continue;
            if (parse_line(line, out)) return true;
        }
        return false;
    }

    // Read entire file into a vector
    std::vector<OrderEvent> read_all() {
        std::vector<OrderEvent> events;
        OrderEvent e{};
        while (next(e)) events.push_back(e);
        return events;
    }

private:
    bool parse_line(const std::string& line, OrderEvent& e) {
        std::vector<std::string> fields;
        std::stringstream ss(line);
        std::string field;
        while (std::getline(ss, field, ','))
            fields.push_back(field);

        int max_col = std::max<int>({cols_.timestamp, cols_.symbol, cols_.order_id,
                                cols_.side, cols_.price, cols_.quantity, cols_.msg_type});
        if ((int)fields.size() <= max_col) return false;

        // ── timestamp ─────────────────────────────────────────────────────────
        // Accept either nanosecond integer or "YYYY-MM-DD HH:MM:SS.nnnnnnnnn"
        const std::string& ts_str = fields[cols_.timestamp];
        if (ts_str.find('-') != std::string::npos) {
            // date-time string — parse to ns since epoch (simplified)
            e.timestamp_ns = parse_datetime_ns(ts_str);
        } else {
            e.timestamp_ns = std::stoull(ts_str);
        }

        // ── symbol ────────────────────────────────────────────────────────────
        std::memset(e.symbol, 0, 8);
        std::memcpy(e.symbol, fields[cols_.symbol].c_str(),
                    std::min<size_t>(7, fields[cols_.symbol].size()));

        // ── order_id ──────────────────────────────────────────────────────────
        e.order_id = std::stoull(fields[cols_.order_id]);

        // ── side ──────────────────────────────────────────────────────────────
        e.side = (uint8_t)std::toupper(fields[cols_.side][0]);
        if (e.side != 'B' && e.side != 'S') return false;

        // ── price → paise (NO FLOAT — pure string arithmetic) ─────────────────
        e.price = snap_to_tick(rupees_to_paise(fields[cols_.price]));

        // ── quantity ──────────────────────────────────────────────────────────
        e.quantity = std::stoi(fields[cols_.quantity]);
        if (e.quantity <= 0) return false;

        // ── msg_type ──────────────────────────────────────────────────────────
        e.msg_type = (uint8_t)std::toupper(fields[cols_.msg_type][0]);
        if (e.msg_type != 'A' && e.msg_type != 'D' &&
            e.msg_type != 'E' && e.msg_type != 'X') return false;

        return true;
    }

    // Parse "2024-01-15 09:15:00.123456789" → nanoseconds since epoch
    // Uses only integer arithmetic — no floating point
    uint64_t parse_datetime_ns(const std::string& s) {
        // minimal parser — good enough for NSE timestamps
        // format: YYYY-MM-DD HH:MM:SS[.nnnnnnnnn]
        if (s.size() < 19) return 0;

        int year  = std::stoi(s.substr(0,  4));
        int month = std::stoi(s.substr(5,  2));
        int day   = std::stoi(s.substr(8,  2));
        int hour  = std::stoi(s.substr(11, 2));
        int min   = std::stoi(s.substr(14, 2));
        int sec   = std::stoi(s.substr(17, 2));

        uint64_t ns_frac = 0;
        if (s.size() > 19 && s[19] == '.') {
            std::string frac = s.substr(20);
            // pad or trim to 9 digits
            while (frac.size() < 9) frac += "0";
            frac = frac.substr(0, 9);
            ns_frac = std::stoull(frac);
        }

        // days since Unix epoch (1970-01-01) — simplified Gregorian
        // accurate for years 2000-2099
        int y = year, m = month, d = day;
        int days = (y - 1970) * 365 + (y - 1969) / 4;
        int month_days[] = {0,31,59,90,120,151,181,212,243,273,304,334};
        days += month_days[m - 1] + (d - 1);
        if (m > 2 && (y % 4 == 0)) days++; // leap year

        uint64_t total_ns = (uint64_t)days   * 86400ULL * 1000000000ULL
                          + (uint64_t)hour   * 3600ULL  * 1000000000ULL
                          + (uint64_t)min    * 60ULL    * 1000000000ULL
                          + (uint64_t)sec               * 1000000000ULL
                          + ns_frac;
        return total_ns;
    }

    std::ifstream file_;
    ColMap cols_;
};