#!/bin/bash
#
# End-of-Day Data Update Script
# Updates daily_high_low.csv (automated); checks NH_NL.csv freshness only --
# NH-NL has no automated source (see step 2 below), it's refreshed manually.
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
echo "2. NH-NL data freshness check"
echo "   NH-NL has no automated source. Refresh manually: export the"
echo "   \$USHL5 (NYSE High-Low Index) series from StockCharts.com, then"
echo "   convert it into data/NH_NL.csv's date,nh_nl_daily,nh_nl_weekly,"
echo "   sp500_close format and copy to /mnt/c/Trading/data/NH_NL.csv."
echo "   This step only WARNS on staleness -- it does not fail the script,"
echo "   since daily_high_low.csv's automated update above is unaffected."

python3 scripts/check_nh_nl_freshness.py \
    --input data/NH_NL.csv \
    --max-age-days 5 \
    --no-strict || true

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

    if [ -f "data/NH_NL.csv" ]; then
        cp -v data/NH_NL.csv "$WIN_DATA_DIR/"
        echo "✅ Copied NH_NL.csv to $WIN_DATA_DIR"
    fi
fi
