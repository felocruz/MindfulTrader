#!/usr/bin/env python3
"""Export NH-NL breadth history from Norgate Data into project CSV contract.

Output schema:
date,nh_nyse,nl_nyse,diff_nyse,nh_nasdaq,nl_nasdaq,diff_nasdaq
"""

from __future__ import annotations

import argparse
import datetime as dt
import sys
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Export Norgate NH-NL data to nh_nl_historical.csv"
    )
    parser.add_argument(
        "--nh-nyse-symbol",
        required=True,
        help="Norgate symbol for NYSE New Highs",
    )
    parser.add_argument(
        "--nl-nyse-symbol",
        required=True,
        help="Norgate symbol for NYSE New Lows",
    )
    parser.add_argument(
        "--nh-nasdaq-symbol",
        required=True,
        help="Norgate symbol for Nasdaq New Highs",
    )
    parser.add_argument(
        "--nl-nasdaq-symbol",
        required=True,
        help="Norgate symbol for Nasdaq New Lows",
    )
    parser.add_argument(
        "--start-date",
        default="1990-01-01",
        help="Start date (YYYY-MM-DD)",
    )
    parser.add_argument(
        "--end-date",
        default=dt.date.today().isoformat(),
        help="End date (YYYY-MM-DD)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("data/nh_nl_historical.csv"),
        help="Output CSV path",
    )
    return parser.parse_args()


def _fetch_close_series(norgatedata, symbol: str, start_date: str, end_date: str):
    frame = norgatedata.price_timeseries(
        symbol,
        timeseriesformat="pandas-dataframe",
        start_date=start_date,
        end_date=end_date,
        padding_setting=norgatedata.PaddingType.NONE,
        interval="D",
    )
    if frame is None or frame.empty or "Close" not in frame.columns:
        raise ValueError(f"No usable Close data for symbol: {symbol}")
    series = frame["Close"].copy()
    series.name = symbol
    return series


def _to_int_series(series):
    return series.fillna(0).round().astype("int64")


def main() -> None:
    args = parse_args()

    try:
        import pandas as pd
        import norgatedata
    except Exception as exc:
        print(f"❌ Required package missing or unavailable: {exc}")
        print("   Ensure Norgate Data Updater is running on Windows and package is installed")
        sys.exit(2)

    if not norgatedata.status():
        print("❌ Norgate Data Updater not detected (norgatedata.status() is False)")
        sys.exit(2)

    try:
        nh_nyse = _fetch_close_series(
            norgatedata, args.nh_nyse_symbol, args.start_date, args.end_date
        )
        nl_nyse = _fetch_close_series(
            norgatedata, args.nl_nyse_symbol, args.start_date, args.end_date
        )
        nh_nasdaq = _fetch_close_series(
            norgatedata, args.nh_nasdaq_symbol, args.start_date, args.end_date
        )
        nl_nasdaq = _fetch_close_series(
            norgatedata, args.nl_nasdaq_symbol, args.start_date, args.end_date
        )
    except Exception as exc:
        print(f"❌ Failed to fetch one or more Norgate series: {exc}")
        sys.exit(2)

    aligned = pd.concat([nh_nyse, nl_nyse, nh_nasdaq, nl_nasdaq], axis=1, join="inner")
    aligned = aligned.dropna(how="any")
    if aligned.empty:
        print("❌ No overlapping rows across NH/NL series after alignment")
        sys.exit(2)

    nh_nyse_i = _to_int_series(aligned[args.nh_nyse_symbol])
    nl_nyse_i = _to_int_series(aligned[args.nl_nyse_symbol])
    nh_nasdaq_i = _to_int_series(aligned[args.nh_nasdaq_symbol])
    nl_nasdaq_i = _to_int_series(aligned[args.nl_nasdaq_symbol])

    out = pd.DataFrame(
        {
            "date": aligned.index.strftime("%Y-%m-%d"),
            "nh_nyse": nh_nyse_i.values,
            "nl_nyse": nl_nyse_i.values,
            "diff_nyse": (nh_nyse_i - nl_nyse_i).values,
            "nh_nasdaq": nh_nasdaq_i.values,
            "nl_nasdaq": nl_nasdaq_i.values,
            "diff_nasdaq": (nh_nasdaq_i - nl_nasdaq_i).values,
        }
    )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    out.to_csv(args.output, index=False)

    first_date = out.iloc[0]["date"]
    last_date = out.iloc[-1]["date"]
    print("✅ Norgate NH-NL export complete")
    print(f"   Output: {args.output}")
    print(f"   Rows: {len(out):,}")
    print(f"   Date range: {first_date} -> {last_date}")


if __name__ == "__main__":
    main()
