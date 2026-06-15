#!/usr/bin/env python3
"""
Download real NSE data using jugaad-data (no API key needed).
Converts OHLCV bars into order-event format for the C++ pipeline.

pip install jugaad-data
"""

import os
import csv
import math
import random
import argparse
from datetime import date, timedelta, datetime

def download_nifty_ohlcv(from_date: date, to_date: date) -> list:
    """Download Nifty 50 index OHLCV via jugaad-data."""
    from jugaad_data.nse import stock_df, index_df
    try:
        # Try Nifty index data first
        df = index_df(symbol="NIFTY 50",
                      from_date=from_date,
                      to_date=to_date)
        print(f"✓ Downloaded {len(df)} Nifty 50 index bars")
        return df
    except Exception as e:
        print(f"Index download failed ({e}), trying NIFTY futures...")
        # Fallback: download a liquid Nifty 50 constituent (Reliance)
        df = stock_df(symbol="RELIANCE",
                      from_date=from_date,
                      to_date=to_date,
                      series="EQ")
        print(f"✓ Downloaded {len(df)} RELIANCE bars (Nifty constituent)")
        return df


# Replace the entire ohlcv_to_order_events function with this fixed version

def ohlcv_to_order_events(df, output_path: str, ticks_per_bar: int = 2000):
    import pandas as pd
    from datetime import datetime as dt, timedelta, date
    import os, csv, random

    os.makedirs(os.path.dirname(output_path) if os.path.dirname(output_path) else '.', exist_ok=True)

    def snap_tick(price):
        return round(round(float(price) / 0.05) * 0.05, 2)

    def to_pydatetime(val):
        if isinstance(val, dt):
            return val
        if isinstance(val, date) and not isinstance(val, dt):
            return dt(val.year, val.month, val.day, 9, 15, 0)
        try:
            return pd.Timestamp(val).to_pydatetime()
        except Exception:
            return dt(2024, 1, 15, 9, 15, 0)

    random.seed(42)

    df.columns = [c.strip() for c in df.columns]
    col_map = {}
    for col in df.columns:
        cl = col.lower().strip()
        if cl in ('open', 'open price'): col_map['open'] = col
        if cl in ('high', 'high price'): col_map['high'] = col
        if cl in ('low', 'low price'): col_map['low'] = col
        if cl in ('close', 'close price'): col_map['close'] = col
        if cl in ('volume', 'tottrdqty', 'no of shares', 'shares traded', 'no. of shares'):
            col_map['volume'] = col

    print(f"Detected columns: {col_map}")

    order_id = 100000
    live_bids = {}   # order_id -> {price, qty}
    live_asks = {}   # order_id -> {price, qty}
    total_rows = 0

    with open(output_path, 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['timestamp', 'symbol', 'order_id', 'side', 'price', 'quantity', 'msg_type'])

        for raw_idx, row in df.iterrows():
            try:
                o = snap_tick(row[col_map['open']])
                h = snap_tick(row[col_map['high']])
                l = snap_tick(row[col_map['low']])
                c = snap_tick(row[col_map['close']])
                vol = int(row.get(col_map.get('volume', ''), 10000) or 10000)
                ts_base = to_pydatetime(raw_idx)
            except Exception as e:
                print(f"Skipping row {raw_idx}: {e}")
                continue

            # Mid-price path
            path = [o]
            if random.random() < 0.5:
                path += [h, l, c]
            else:
                path += [l, h, c]

            bar_seconds = max(60, 375 * 60 // max(len(df), 1))
            steps = ticks_per_bar
            avg_qty = max(1, vol // max(steps, 1))

            # Seed both sides so the book starts valid
            start_mid = path[0]
            start_mid_paise = int(round(start_mid * 100))
            start_mid_tick = start_mid_paise // 5

            for offset in [2, 3, 4]:
                bid_tick = start_mid_tick - offset
                ask_tick = start_mid_tick + offset

                bid_price = bid_tick * 5 / 100.0
                ask_price = ask_tick * 5 / 100.0

                order_id += 1
                bqty = max(1, int(abs(random.gauss(avg_qty, max(avg_qty * 0.5, 1)))))
                writer.writerow([
                    (ts_base + timedelta(milliseconds=offset)).strftime("%Y-%m-%d %H:%M:%S.") + f"{random.randint(0,999999999):09d}",
                    "RELIANCE", order_id, "B", f"{bid_price:.2f}", bqty, "A"
                ])
                live_bids[order_id] = {"price": bid_price, "qty": bqty}
                total_rows += 1

                order_id += 1
                aqty = max(1, int(abs(random.gauss(avg_qty, max(avg_qty * 0.5, 1)))))
                writer.writerow([
                    (ts_base + timedelta(milliseconds=offset+10)).strftime("%Y-%m-%d %H:%M:%S.") + f"{random.randint(0,999999999):09d}",
                    "RELIANCE", order_id, "S", f"{ask_price:.2f}", aqty, "A"
                ])
                live_asks[order_id] = {"price": ask_price, "qty": aqty}
                total_rows += 1

            t_offset_sec = 0.0

            for step in range(steps):
                frac_global = step / max(steps - 1, 1)

                seg = min(int(frac_global * (len(path) - 1)), len(path) - 2)
                local_start = path[seg]
                local_end = path[seg + 1]

                seg_start_frac = seg / (len(path) - 1)
                seg_end_frac = (seg + 1) / (len(path) - 1)
                local_frac = 0.0 if seg_end_frac == seg_start_frac else \
                             (frac_global - seg_start_frac) / (seg_end_frac - seg_start_frac)

                mid = snap_tick(local_start + local_frac * (local_end - local_start))
                mid_paise = int(round(mid * 100))
                mid_tick = mid_paise // 5

                # positive spread: 2–4 ticks, never locked
                spread_ticks = random.choice([2, 2, 3, 3, 4])

                side = 'B' if random.random() < 0.5 else 'S'
                depth_offset = random.choice([0, 1, 1, 2, 3])

                if side == 'B':
                    price_tick = mid_tick - spread_ticks - depth_offset
                    price = price_tick * 5 / 100.0
                else:
                    price_tick = mid_tick + spread_ticks + depth_offset
                    price = price_tick * 5 / 100.0

                qty = max(1, min(500, int(abs(random.gauss(avg_qty, max(avg_qty * 0.5, 1))))))

                t_offset_sec += random.expovariate(steps / bar_seconds)
                safe_offset = min(t_offset_sec, bar_seconds - 0.001)
                ts = ts_base + timedelta(seconds=safe_offset)
                ts_str = ts.strftime("%Y-%m-%d %H:%M:%S.") + f"{random.randint(0,999999999):09d}"

                r = random.random()

                if r < 0.60:
                    order_id += 1
                    writer.writerow([ts_str, "RELIANCE", order_id, side, f"{price:.2f}", qty, "A"])
                    if side == 'B':
                        live_bids[order_id] = {"price": price, "qty": qty}
                    else:
                        live_asks[order_id] = {"price": price, "qty": qty}
                    total_rows += 1

                elif r < 0.85:
                    pool = live_bids if side == 'B' else live_asks
                    if pool:
                        oid = random.choice(list(pool.keys()))
                        info = pool.pop(oid)
                        writer.writerow([ts_str, "RELIANCE", oid, side, f"{info['price']:.2f}", info['qty'], "D"])
                        total_rows += 1

                elif r < 0.95:
                    pool = live_bids if side == 'B' else live_asks
                    if pool:
                        oid = random.choice(list(pool.keys()))
                        info = pool[oid]
                        exec_qty = max(1, int(info['qty'] * random.uniform(0.2, 1.0)))
                        writer.writerow([ts_str, "RELIANCE", oid, side, f"{info['price']:.2f}", exec_qty, "E"])
                        info['qty'] -= exec_qty
                        if info['qty'] <= 0:
                            del pool[oid]
                        total_rows += 1

                else:
                    pool = live_bids if side == 'B' else live_asks
                    if pool:
                        oid = random.choice(list(pool.keys()))
                        info = pool[oid]
                        cxl_qty = max(1, int(info['qty'] * random.uniform(0.1, 0.4)))
                        writer.writerow([ts_str, "RELIANCE", oid, side, f"{info['price']:.2f}", cxl_qty, "X"])
                        info['qty'] -= cxl_qty
                        if info['qty'] <= 0:
                            del pool[oid]
                        total_rows += 1

    print(f"✓ Wrote {total_rows} order events → {output_path}")
    return total_rows


def download_bhavcopy_approach(output_path: str):
    """
    Alternative: download NSE Bhavcopy (end-of-day snapshot for all symbols)
    and extract Nifty 50 constituents. Gives you real closing prices.
    """
    from jugaad_data.nse import bhavcopy_save
    import zipfile
    import io

    bhavcopy_dir = "data/bhavcopy"
    os.makedirs(bhavcopy_dir, exist_ok=True)

    # Download last 5 trading days of bhavcopy
    today = date.today()
    downloaded = []
    attempts   = 0
    d = today - timedelta(days=1)

    print("Downloading NSE Bhavcopy files...")
    while len(downloaded) < 5 and attempts < 14:
        if d.weekday() < 5:  # skip weekends
            try:
                bhavcopy_save(d, bhavcopy_dir)
                downloaded.append(d)
                print(f"  ✓ {d}")
            except Exception as e:
                print(f"  ✗ {d} — {e}")
        d -= timedelta(days=1)
        attempts += 1

    if not downloaded:
        print("Could not download any bhavcopy files. Using synthetic fallback.")
        return False

    print(f"\nDownloaded {len(downloaded)} bhavcopy files.")
    print("Converting to order-event format...")

    # Parse all downloaded CSV files
    import pandas as pd
    import glob

    all_dfs = []
    for csv_file in glob.glob(f"{bhavcopy_dir}/*.csv"):
        try:
            df = pd.read_csv(csv_file)
            # Bhavcopy columns: SYMBOL, SERIES, OPEN, HIGH, LOW, CLOSE, ...
            df.columns = [c.strip() for c in df.columns]
            all_dfs.append(df)
        except Exception:
            pass

    if not all_dfs:
        return False

    import pandas as pd
    combined = pd.concat(all_dfs, ignore_index=True)

    # Filter for Nifty 50 index or high-liquidity EQ series
    if 'SERIES' in combined.columns:
        combined = combined[combined['SERIES'].str.strip() == 'EQ']

    print(f"  Total records: {len(combined)}")

    # Use top-10 liquid Nifty 50 stocks by name match
    nifty50_symbols = [
        'RELIANCE', 'TCS', 'HDFCBANK', 'INFY', 'HINDUNILVR',
        'ICICIBANK', 'KOTAKBANK', 'SBIN', 'BAJFINANCE', 'BHARTIARTL'
    ]
    sym_col = 'SYMBOL' if 'SYMBOL' in combined.columns else combined.columns[0]
    filtered = combined[combined[sym_col].isin(nifty50_symbols)]

    if len(filtered) == 0:
        print("  No Nifty 50 symbols found in bhavcopy. Using all symbols.")
        filtered = combined.head(500)

    print(f"  Nifty 50 records: {len(filtered)}")

    # Convert to OHLCV-like dataframe with DatetimeIndex
    col_aliases = {
        'OPEN': 'open', 'HIGH': 'high', 'LOW': 'low', 'CLOSE': 'close',
        'TOTTRDQTY': 'volume', 'NO. OF SHARES': 'volume'
    }
    filtered = filtered.rename(columns=col_aliases)
    filtered.index = pd.to_datetime(filtered.get('TIMESTAMP', date.today().isoformat()))

    return ohlcv_to_order_events(filtered, output_path, ticks_per_bar=100)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Download real NSE data via jugaad-data")
    parser.add_argument('--mode', choices=['ohlcv', 'bhavcopy', 'synthetic'],
                        default='ohlcv',
                        help='ohlcv: download index/stock bars | '
                             'bhavcopy: download EOD snapshots | '
                             'synthetic: no download needed')
    parser.add_argument('--days', type=int, default=5,
                        help='Number of trading days to download')
    parser.add_argument('--output', default='data/nse_ticks.csv')
    args = parser.parse_args()

    os.makedirs('data', exist_ok=True)

    if args.mode == 'ohlcv':
        print("Downloading Nifty OHLCV via jugaad-data...")
        to_dt   = date.today() - timedelta(days=1)
        from_dt = to_dt - timedelta(days=args.days + 4)  # +4 for weekends
        df = download_nifty_ohlcv(from_dt, to_dt)
        if df is not None and len(df) > 0:
            ohlcv_to_order_events(df, args.output, ticks_per_bar=2000)
        else:
            print("Download failed. Run with --mode synthetic as fallback.")

    elif args.mode == 'bhavcopy':
        print("Downloading NSE Bhavcopy...")
        download_bhavcopy_approach(args.output)

    elif args.mode == 'synthetic':
        # Import and run the original synthetic generator
        import sys
        sys.path.insert(0, '.')
        n_rows = 50000
        random.seed(42)

        ref_price = 22500.0
        tick = 0.05
        from datetime import datetime
        base_time = datetime(2024, 1, 15, 9, 15, 0)

        live_orders = {}
        order_id = 100000
        current_price = ref_price

        def snap(p): return round(round(p / tick) * tick, 2)

        with open(args.output, 'w', newline='') as f:
            writer = csv.writer(f)
            writer.writerow(['timestamp', 'symbol', 'order_id', 'side',
                             'price', 'quantity', 'msg_type'])
            written = 0
            t_offset = 0.0
            while written < n_rows:
                t_offset += random.expovariate(500.0)
                if t_offset > 22500: break
                ts = base_time + timedelta(seconds=t_offset)
                drift = (ref_price - current_price) * 0.001
                current_price = snap(current_price + drift + random.gauss(0, 0.15))
                current_price = max(22000.0, min(23000.0, current_price))
                r = random.random()
                if r < 0.60 or len(live_orders) < 10:
                    order_id += 1
                    side  = 'B' if random.random() < 0.5 else 'S'
                    price = snap(current_price + random.gauss(0, 1.5))
                    qty   = max(1, min(500, int(random.lognormvariate(math.log(25), 1.2))))
                    ts_str = ts.strftime("%Y-%m-%d %H:%M:%S.") + f"{random.randint(0,999999999):09d}"
                    writer.writerow([ts_str, 'NIFTY50', order_id, side, f"{price:.2f}", qty, 'A'])
                    live_orders[order_id] = {'side': side, 'price': price, 'qty': qty}
                elif r < 0.85 and live_orders:
                    oid = random.choice(list(live_orders.keys()))
                    info = live_orders.pop(oid)
                    ts_str = ts.strftime("%Y-%m-%d %H:%M:%S.") + f"{random.randint(0,999999999):09d}"
                    writer.writerow([ts_str, 'NIFTY50', oid, info['side'], f"{info['price']:.2f}", info['qty'], 'D'])
                elif r < 0.95 and live_orders:
                    oid = random.choice(list(live_orders.keys()))
                    info = live_orders[oid]
                    eq = max(1, int(info['qty'] * random.uniform(0.2, 1.0)))
                    ts_str = ts.strftime("%Y-%m-%d %H:%M:%S.") + f"{random.randint(0,999999999):09d}"
                    writer.writerow([ts_str, 'NIFTY50', oid, info['side'], f"{info['price']:.2f}", eq, 'E'])
                    info['qty'] -= eq
                    if info['qty'] <= 0: del live_orders[oid]
                else:
                    if live_orders:
                        oid = random.choice(list(live_orders.keys()))
                        info = live_orders[oid]
                        cq = max(1, int(info['qty'] * random.uniform(0.1, 0.4)))
                        ts_str = ts.strftime("%Y-%m-%d %H:%M:%S.") + f"{random.randint(0,999999999):09d}"
                        writer.writerow([ts_str, 'NIFTY50', oid, info['side'], f"{info['price']:.2f}", cq, 'X'])
                        info['qty'] -= cq
                        if info['qty'] <= 0: del live_orders[oid]
                written += 1
        print(f"✓ Generated {written} synthetic rows → {args.output}")