#!/usr/bin/env python
# imbalance_triple_screen_first_6mo.py -- one-off visual: IS1/IS2/IS3 hierarchical
# imbalance bars (docs/superpowers/specs/2026-09-18-predator-sniper-execution-architecture.md
# §1.2a/§1.2b), built over the first 6 months of real MES ticks. IS3 = adaptive-threshold
# imbalance bars (AFML Ch.2 EWMA, mirrors include/ImbalanceBarEngine.h exactly); IS2 = every
# K2=4 completed IS3 bars aggregated into one; IS1 = every K1=4 completed IS2 bars aggregated
# into one (K=4 empirically validated 2026-09-07, tools/observation_vector/
# imbalance_clock_manager_ratio_eval.cpp). Line charts, not candlesticks -- IS3 alone has tens
# of thousands of bars over 6 months, too dense to render as OHLC bars usefully.
#
# Run: mamba run -n mts python tools/visualization/imbalance_triple_screen_first_6mo.py

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
import pyarrow.parquet as pq


TICKS_PATH = "/home/rcruz/devel/VSCode/lbrnet/data/raw/mes_ticks.parquet"
OUT_DIR = "tools/output"
FIRST_ROW_GROUP = 0
LAST_ROW_GROUP = 783  # 2023-06-04 .. ~2023-12-04, ~6 months (found via binary search, 2026-09-07)
ADAPTIVE_ALPHA = 0.01
K2 = 4
K1 = 4


def load_first_6_months():
    f = pq.ParquetFile(TICKS_PATH)
    cols = ["timestamp_us", "trade_price", "volume", "bid_volume", "ask_volume"]
    tables = [
        f.read_row_group(rg, columns=cols) for rg in range(FIRST_ROW_GROUP, LAST_ROW_GROUP + 1)
    ]
    df = pd.concat([t.to_pandas() for t in tables], ignore_index=True)
    df["signed_flow"] = df["ask_volume"] - df["bid_volume"]
    return df


def build_is3_adaptive_bars(prices, flows):
    # Mirrors include/ImbalanceBarEngine.h's OnTickWithPrice + EnableAdaptiveThreshold exactly:
    # b_target = E[T]*|E[b_k*v_k]| (AFML Ch.2, Lopez de Prado 2018), EWMA-updated after every
    # completed bar, seeded from the first real completed bar's own values.
    closes = []
    cum = 0.0
    open_px = None
    ticks_in_bar = 0
    have_estimate = False
    ewma_ticks = 0.0
    ewma_imbalance = 0.0
    fixed_fallback = 700.0

    for price, flow in zip(prices, flows):
        if open_px is None:
            open_px = price
        cum += flow
        ticks_in_bar += 1

        threshold = (ewma_ticks * abs(ewma_imbalance)) if have_estimate else fixed_fallback
        if abs(cum) >= threshold:
            closes.append(price)
            per_tick = cum / ticks_in_bar
            if not have_estimate:
                ewma_ticks = float(ticks_in_bar)
                ewma_imbalance = per_tick
                have_estimate = True
            else:
                ewma_ticks += ADAPTIVE_ALPHA * (ticks_in_bar - ewma_ticks)
                ewma_imbalance += ADAPTIVE_ALPHA * (per_tick - ewma_imbalance)
            cum = 0.0
            open_px = None
            ticks_in_bar = 0
    return np.array(closes)


def aggregate(closes, k):
    # One aggregated bar's close = the last constituent bar's close (exact, not approximate --
    # matches ImbalanceClockManager.h's own return-summing property: only every k-th close is kept).
    n = (len(closes) // k) * k
    return closes[:n].reshape(-1, k)[:, -1]


def main():
    print(f"loading row groups {FIRST_ROW_GROUP}..{LAST_ROW_GROUP} (~first 6 months)...")
    df = load_first_6_months()
    print(
        f"loaded {len(df):,} ticks, {df['timestamp_us'].iloc[0]} .. {df['timestamp_us'].iloc[-1]} (us epoch)"
    )

    prices = df["trade_price"].to_numpy()
    flows = df["signed_flow"].to_numpy()

    print("building IS3 (adaptive threshold) bars...")
    is3_closes = build_is3_adaptive_bars(prices, flows)
    print(f"IS3 bars: {len(is3_closes):,}")

    is2_closes = aggregate(is3_closes, K2)
    is1_closes = aggregate(is2_closes, K1)
    print(f"IS2 bars: {len(is2_closes):,}  (K2={K2})")
    print(f"IS1 bars: {len(is1_closes):,}  (K1={K1})")

    fig, axes = plt.subplots(3, 1, figsize=(14, 10), sharex=False)
    for ax, (label, closes) in zip(
        axes,
        [
            ("IS1 (macro)", is1_closes),
            ("IS2 (intermediate)", is2_closes),
            ("IS3 (micro)", is3_closes),
        ],
    ):
        ax.plot(closes, linewidth=0.6)
        ax.set_title(f"{label} -- {len(closes):,} bars, first 6 months (2023-06-04..~2023-12-04)")
        ax.set_ylabel("Close")
        ax.set_xlabel("Bar index")

    fig.tight_layout()
    out_path = f"{OUT_DIR}/imbalance_triple_screen_first_6mo.png"
    fig.savefig(out_path, dpi=120)
    print(f"saved {out_path}")


if __name__ == "__main__":
    main()
