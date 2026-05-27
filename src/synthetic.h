#pragma once
#include "../include/types.h"
#include <random>
#include <chrono>
#include <vector>
#include <cstring>

// ── SyntheticGenerator ────────────────────────────────────────────────────────
// Produces realistic NSE-calibrated OrderEvents.
//
// Parameters tuned to Nifty 50 microstructure:
//   - Reference price ~₹22,500 (Nifty spot level)
//   - Tick size: 5 paise
//   - Bid-ask spread: 1-3 ticks (5-15 paise)
//   - Order arrivals: Poisson process, ~500 orders/second during market hours
//   - Quantity: 1-200 units, log-normal distributed
//   - Message type distribution: ~60% Add, ~25% Delete, ~10% Execute, ~5% Cancel

class SyntheticGenerator {
public:
    struct Config {
        Config() = default;
        int64_t  ref_price_paise = 22500'00;  // ₹22,500.00 — Nifty reference
        int64_t  price_std_paise = 50'00;     // ₹50 std dev for price spread around ref
        double   arrival_rate    = 500.0;     // orders per second
        int64_t  tick_size       = 5;         // 5 paise = ₹0.05
        int32_t  max_quantity    = 200;
        uint64_t start_time_ns   = 0;         // 0 = use current wall clock
        char     symbol[8]       = "NIFTY50";
    };

    explicit SyntheticGenerator()
        : SyntheticGenerator(Config{}) {}

    explicit SyntheticGenerator(Config cfg)
        : cfg_(cfg)
        , rng_(std::random_device{}())
        , price_dist_(0.0, (double)cfg.price_std_paise)
        , qty_dist_(1.0, 1.5)                  // log-normal shape params
        , interarrival_dist_(cfg.arrival_rate)  // Poisson → exponential interarrival
        , side_dist_(0.0, 1.0)
        , type_dist_(0.0, 1.0)
        , next_order_id_(1)
    {
        if (cfg_.start_time_ns == 0) {
            cfg_.start_time_ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        }
        current_time_ns_ = cfg_.start_time_ns;

        // seed the live order pool with some initial orders so deletes/executes
        // have valid order_ids to reference from the start
        for (int i = 0; i < 50; ++i) {
            live_orders_.push_back({next_order_id_++, random_price(), random_qty(),
                                    (side_dist_(rng_) < 0.5) ? 'B' : 'S'});
        }
    }

    // Generate the next OrderEvent. Call repeatedly.
    OrderEvent next() {
        // advance simulated time by one Poisson interarrival interval
        double interarrival_sec = std::exponential_distribution<double>(cfg_.arrival_rate)(rng_);
        current_time_ns_ += (uint64_t)(interarrival_sec * 1e9);

        OrderEvent e{};
        std::memcpy(e.symbol, cfg_.symbol, 8);
        e.timestamp_ns = current_time_ns_;

        double r = type_dist_(rng_);
        if (r < 0.60 || live_orders_.empty()) {
            make_add(e);
        } else if (r < 0.85) {
            make_delete(e);
        } else if (r < 0.95) {
            make_execute(e);
        } else {
            make_cancel(e);
        }
        return e;
    }

    // Generate n events at once
    std::vector<OrderEvent> generate(size_t n) {
        std::vector<OrderEvent> out;
        out.reserve(n);
        for (size_t i = 0; i < n; ++i) out.push_back(next());
        return out;
    }

private:
    struct LiveOrder {
        uint64_t order_id;
        int64_t  price;
        int32_t  qty;
        char     side;
    };

    int64_t random_price() {
        int64_t raw = cfg_.ref_price_paise + (int64_t)price_dist_(rng_);
        return snap_to_tick(raw);
    }

    int32_t random_qty() {
        double lnorm = std::lognormal_distribution<double>(
            std::log(20.0), qty_dist_.param().m())(rng_);
        int32_t q = std::max(1, (int32_t)lnorm);
        return std::min(q, cfg_.max_quantity);
    }

    void make_add(OrderEvent& e) {
        e.msg_type  = 'A';
        e.order_id  = next_order_id_++;
        e.side      = (side_dist_(rng_) < 0.5) ? 'B' : 'S';
        e.price     = random_price();
        e.quantity  = random_qty();
        live_orders_.push_back({e.order_id, e.price, e.quantity, (char)e.side});
    }

    void make_delete(OrderEvent& e) {
        size_t idx = pick_live_idx();
        auto& lo   = live_orders_[idx];
        e.msg_type  = 'D';
        e.order_id  = lo.order_id;
        e.side      = lo.side;
        e.price     = lo.price;
        e.quantity  = lo.qty;
        live_orders_.erase(live_orders_.begin() + idx);
    }

    void make_execute(OrderEvent& e) {
        size_t idx = pick_live_idx();
        auto& lo   = live_orders_[idx];
        // execute partial or full
        int32_t exec_qty = std::max(1, (int32_t)(lo.qty * side_dist_(rng_)));
        e.msg_type  = 'E';
        e.order_id  = lo.order_id;
        e.side      = lo.side;
        e.price     = lo.price;
        e.quantity  = exec_qty;
        lo.qty -= exec_qty;
        if (lo.qty <= 0)
            live_orders_.erase(live_orders_.begin() + idx);
    }

    void make_cancel(OrderEvent& e) {
        // cancel = partial delete — same structure as delete, labelled differently
        size_t idx = pick_live_idx();
        auto& lo   = live_orders_[idx];
        int32_t cancel_qty = std::max(1, (int32_t)(lo.qty * side_dist_(rng_)));
        e.msg_type  = 'X';
        e.order_id  = lo.order_id;
        e.side      = lo.side;
        e.price     = lo.price;
        e.quantity  = cancel_qty;
        lo.qty -= cancel_qty;
        if (lo.qty <= 0)
            live_orders_.erase(live_orders_.begin() + idx);
    }

    size_t pick_live_idx() {
        std::uniform_int_distribution<size_t> d(0, live_orders_.size() - 1);
        return d(rng_);
    }

    Config              cfg_;
    std::mt19937_64     rng_;
    std::normal_distribution<double>      price_dist_;
    std::lognormal_distribution<double>   qty_dist_;
    std::exponential_distribution<double> interarrival_dist_;
    std::uniform_real_distribution<double> side_dist_;
    std::uniform_real_distribution<double> type_dist_;
    uint64_t            next_order_id_;
    uint64_t            current_time_ns_;
    std::vector<LiveOrder> live_orders_;
};