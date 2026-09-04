#!/usr/bin/env python
# imbalance_vs_time_bars.py -- one-off visual comparison: does a human still
# "see" swing high/low patterns on an order-flow imbalance-bar chart the same
# way they do on a classic 15-min time-bar chart? Real MES ticks, same window,
# both clocks, swing-high/low + tail-move markers on both for direct comparison.
#
# Run: mamba run -n mts python tools/visualization/imbalance_vs_time_bars.py

import numpy as np
import pandas as pd
import pyarrow.parquet as pq
import mplfinance as mpf

TICKS_PATH = "/home/rcruz/devel/VSCode/lbrnet/data/raw/mes_ticks.parquet"
OUT_DIR = "tools/output"
SWING_K = 3        # bars on each side for a swing high/low pivot
TAIL_Z = 2.0       # |z| threshold for a "tail move" marker


def load_ticks():
    f = pq.ParquetFile(TICKS_PATH)
    # Row groups around NY open on a real trading day (2025-04-16), found by
    # inspection -- 5 groups ~ a few hours of real activity, good for a chart.
    tables = [f.read_row_group(rg, columns=["timestamp_us", "trade_price", "volume",
                                             "bid_volume", "ask_volume", "contract"])
              for rg in range(3602, 3607)]
    df = pd.concat([t.to_pandas() for t in tables], ignore_index=True)
    df = df[df["contract"] == df["contract"].iloc[0]].reset_index(drop=True)
    df["ts"] = pd.to_datetime(df["timestamp_us"], unit="us", utc=True)
    df["signed_flow"] = df["ask_volume"] - df["bid_volume"]
    return df


def build_time_bars(df, minutes=15):
    bucket = df.set_index("ts").resample(f"{minutes}min")
    bars = bucket["trade_price"].agg(["first", "max", "min", "last"])
    bars.columns = ["Open", "High", "Low", "Close"]
    bars["Volume"] = bucket["volume"].sum()
    return bars.dropna()


def build_imbalance_bars(df, threshold):
    rows = []
    cum = 0.0
    open_px = None
    high_px = -np.inf
    low_px = np.inf
    vol = 0
    bar_start_ts = None
    for ts, price, volume, flow in zip(df["ts"], df["trade_price"], df["volume"], df["signed_flow"]):
        if open_px is None:
            open_px = price
            bar_start_ts = ts
        high_px = max(high_px, price)
        low_px = min(low_px, price)
        vol += volume
        cum += flow
        if abs(cum) >= threshold:
            rows.append((bar_start_ts, open_px, high_px, low_px, price, vol))
            cum = 0.0
            open_px = None
            high_px = -np.inf
            low_px = np.inf
            vol = 0
    bars = pd.DataFrame(rows, columns=["ts", "Open", "High", "Low", "Close", "Volume"]).set_index("ts")
    return bars


def pick_threshold_for_target_bar_count(df, target_bars):
    # Binary search a threshold on cumulative |signed_flow| so the imbalance-bar
    # count lands close to target_bars, for a fair side-by-side density comparison.
    flow = df["signed_flow"].to_numpy()
    lo, hi = 1.0, float(np.abs(flow).sum())
    best_threshold, best_diff = hi, float("inf")
    for _ in range(25):
        mid = (lo + hi) / 2.0
        cum = 0.0
        count = 0
        for f in flow:
            cum += f
            if abs(cum) >= mid:
                cum = 0.0
                count += 1
        diff = abs(count - target_bars)
        if diff < best_diff:
            best_diff, best_threshold = diff, mid
        if count > target_bars:
            lo = mid
        else:
            hi = mid
    return best_threshold


def mark_swings(bars, k=SWING_K):
    highs = bars["High"].to_numpy()
    lows = bars["Low"].to_numpy()
    n = len(bars)
    swing_high = np.full(n, np.nan)
    swing_low = np.full(n, np.nan)
    for i in range(k, n - k):
        window_h = highs[i - k:i + k + 1]
        window_l = lows[i - k:i + k + 1]
        if highs[i] == window_h.max() and np.argmax(window_h) == k:
            swing_high[i] = highs[i] * 1.0015
        if lows[i] == window_l.min() and np.argmin(window_l) == k:
            swing_low[i] = lows[i] * 0.9985
    return swing_high, swing_low


def mark_tail_moves(bars, z_thresh=TAIL_Z):
    ret = np.log(bars["Close"] / bars["Open"]).to_numpy()
    med = np.median(ret)
    mad = np.median(np.abs(ret - med)) * 1.4826 + 1e-9
    z = (ret - med) / mad
    tail = np.full(len(bars), np.nan)
    hit = np.abs(z) >= z_thresh
    tail[hit] = bars["Close"].to_numpy()[hit]
    return tail, hit.sum()


def render(bars, title, out_path):
    swing_high, swing_low = mark_swings(bars)
    tail, n_tail = mark_tail_moves(bars)
    addplots = [
        mpf.make_addplot(swing_high, type="scatter", markersize=60, marker="v", color="red"),
        mpf.make_addplot(swing_low, type="scatter", markersize=60, marker="^", color="green"),
        mpf.make_addplot(tail, type="scatter", markersize=90, marker="*", color="gold"),
    ]
    mpf.plot(bars, type="candle", style="charles", addplot=addplots, volume=True,
              title=title, savefig=dict(fname=out_path, dpi=150))
    n_swing_high = int(np.sum(~np.isnan(swing_high)))
    n_swing_low = int(np.sum(~np.isnan(swing_low)))
    print(f"{title}: n_bars={len(bars)} swing_highs={n_swing_high} swing_lows={n_swing_low} tail_moves={n_tail}")


def main():
    df = load_ticks()
    print(f"loaded {len(df)} real ticks, {df['ts'].iloc[0]} .. {df['ts'].iloc[-1]}, contract={df['contract'].iloc[0]}")

    time_bars = build_time_bars(df, minutes=15)
    render(time_bars, "15-min TIME bars (real MES, NY open window)", f"{OUT_DIR}/time_bars_chart.png")

    threshold = pick_threshold_for_target_bar_count(df, target_bars=len(time_bars))
    imbalance_bars = build_imbalance_bars(df, threshold)
    print(f"imbalance threshold picked: {threshold:.1f} (targeted {len(time_bars)} bars)")
    render(imbalance_bars, "Order-flow IMBALANCE bars (same real ticks, same window)",
           f"{OUT_DIR}/imbalance_bars_chart.png")


if __name__ == "__main__":
    main()
