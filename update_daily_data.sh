#!/bin/bash
#
# End-of-Day Data Update Script
# Updates both nh_nl_historical.csv and daily_high_low.csv
#
# Usage:
#   ./update_daily_data.sh
#
# Cron job (5pm CT daily):
#   0 17 * * 1-5 /home/rcruz/devel/VSCode/MindfulTrader/update_daily_data.sh >> /var/log/trading/daily_update.log 2>&1
#

set -e  # Exit on error

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Activate mamba environment
eval "$(mamba shell.bash hook)"
mamba activate mts

echo "=========================================="
echo "End-of-Day Data Update"
echo "Date: $(date)"
echo "=========================================="

# Get yesterday's date (trading data is T-1)
YESTERDAY=$(date -d "yesterday" +%Y-%m-%d)
TODAY=$(date +%Y-%m-%d)

echo ""
echo "1. Updating daily high/low data (MES from Interactive Brokers)..."
echo "   Fetching data for: $YESTERDAY to $TODAY"
echo "   Note: Requires TWS or IB Gateway running on port 4002"

# Use Yahoo Finance ES=F (standard approach for MES traders)
# IB has limited historical data (1-2 years), yfinance reliable for multi-year history
# ES and MES have identical price levels (same S&P 500 index)
python populate_daily_high_low_hybrid.py --start-date "$YESTERDAY" --end-date "$TODAY"

if [ $? -eq 0 ]; then
    echo "   ✅ Daily high/low update successful"
else
    echo "   ❌ Daily high/low update failed"
    exit 1
fi

echo ""
echo "2. NH-NL data update"
USE_NORGATE_NHNL="${USE_NORGATE_NHNL:-0}"

if [ "$USE_NORGATE_NHNL" = "1" ]; then
    echo "   Source: Norgate Data"
    : "${NORGATE_NH_NYSE_SYMBOL:?Set NORGATE_NH_NYSE_SYMBOL}"
    : "${NORGATE_NL_NYSE_SYMBOL:?Set NORGATE_NL_NYSE_SYMBOL}"
    : "${NORGATE_NH_NASDAQ_SYMBOL:?Set NORGATE_NH_NASDAQ_SYMBOL}"
    : "${NORGATE_NL_NASDAQ_SYMBOL:?Set NORGATE_NL_NASDAQ_SYMBOL}"

    python3 scripts/export_norgate_nh_nl.py \
        --nh-nyse-symbol "$NORGATE_NH_NYSE_SYMBOL" \
        --nl-nyse-symbol "$NORGATE_NL_NYSE_SYMBOL" \
        --nh-nasdaq-symbol "$NORGATE_NH_NASDAQ_SYMBOL" \
        --nl-nasdaq-symbol "$NORGATE_NL_NASDAQ_SYMBOL" \
        --output data/nh_nl_historical.csv

    python3 scripts/check_nh_nl_freshness.py \
        --input data/nh_nl_historical.csv \
        --max-age-days 5 \
        --strict

    echo "   ✅ NH-NL update successful (Norgate)"
else
    echo "   Skipped automated NH-NL update (USE_NORGATE_NHNL=$USE_NORGATE_NHNL)"
    echo "   To enable Norgate automation, set:"
    echo "     USE_NORGATE_NHNL=1"
    echo "     NORGATE_NH_NYSE_SYMBOL, NORGATE_NL_NYSE_SYMBOL"
    echo "     NORGATE_NH_NASDAQ_SYMBOL, NORGATE_NL_NASDAQ_SYMBOL"
fi

echo ""
echo "=========================================="
echo "End-of-Day Update Complete"
echo "$(date)"
echo "=========================================="

# Optional: Copy data to Windows path if running on WSL
if grep -qi microsoft /proc/version 2>/dev/null; then
    echo ""
    echo "WSL detected - copying to Windows path..."

    WIN_DATA_DIR="/mnt/c/Trading/data"
    mkdir -p "$WIN_DATA_DIR"

    if [ -f "data/daily_high_low.csv" ]; then
        cp -v data/daily_high_low.csv "$WIN_DATA_DIR/"
        echo "✅ Copied daily_high_low.csv to $WIN_DATA_DIR"
    fi

    if [ -f "data/nh_nl_historical.csv" ]; then
        cp -v data/nh_nl_historical.csv "$WIN_DATA_DIR/"
        echo "✅ Copied nh_nl_historical.csv to $WIN_DATA_DIR"
    fi
fi
