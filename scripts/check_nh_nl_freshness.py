#!/usr/bin/env python3
"""Check NH-NL CSV freshness and basic integrity.

Default target (WSL): /mnt/c/Trading/data/NH_NL.csv
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import sys
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Validate NH-NL CSV freshness")
    parser.add_argument(
        "--input",
        type=Path,
        default=Path("/mnt/c/Trading/data/NH_NL.csv"),
        help="Path to NH_NL.csv",
    )
    parser.add_argument(
        "--max-age-days",
        type=int,
        default=5,
        help="Maximum allowed age for the latest CSV date",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        default=True,
        help="Exit non-zero when stale or invalid",
    )
    parser.add_argument("--no-strict", action="store_false", dest="strict")
    return parser.parse_args()


def main() -> None:
    args = parse_args()

    if not args.input.exists():
        print(f"❌ NH-NL CSV not found: {args.input}")
        sys.exit(2 if args.strict else 0)

    first_date: dt.date | None = None
    last_date: dt.date | None = None
    row_count = 0

    with args.input.open("r", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        if "date" not in (reader.fieldnames or []):
            print("❌ CSV missing required 'date' column")
            sys.exit(2 if args.strict else 0)

        for row in reader:
            date_str = (row.get("date") or "").strip()
            if not date_str:
                continue
            try:
                d = dt.datetime.strptime(date_str, "%Y-%m-%d").date()
            except ValueError:
                continue

            row_count += 1
            if first_date is None or d < first_date:
                first_date = d
            if last_date is None or d > last_date:
                last_date = d

    if row_count == 0 or first_date is None or last_date is None:
        print(f"❌ No valid dated rows found in: {args.input}")
        sys.exit(2 if args.strict else 0)

    today = dt.date.today()
    age_days = (today - last_date).days

    print("=" * 72)
    print("NH-NL CSV Freshness Check")
    print("=" * 72)
    print(f"File: {args.input}")
    print(f"Rows: {row_count:,}")
    print(f"Date range: {first_date.isoformat()} -> {last_date.isoformat()}")
    print(f"Age (days): {age_days}")
    print(f"Max allowed age: {args.max_age_days}")

    if age_days > args.max_age_days:
        print("❌ STALE: latest NH-NL date is too old")
        print("   Action: refresh CSV before collecting new .context data")
        sys.exit(2 if args.strict else 0)

    print("✅ PASS: NH-NL CSV freshness is within threshold")


if __name__ == "__main__":
    main()
