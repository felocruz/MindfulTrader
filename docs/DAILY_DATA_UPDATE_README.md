# Daily Data Update Scripts

Automated scripts for populating and updating trading data files required by MindfulTrader C++ studies.

## Overview

Two data files are required:
1. **daily_high_low.csv** - Previous day's high/low for MES futures (automated)
2. **nh_nl_historical.csv** - NYSE new highs/lows breadth data (manual)

### Data Source Options for MES

**Recommended: Yahoo Finance ES=F (Free)**
- ES and MES have identical prices (both CME products, same S&P 500 index)
- Multi-year historical data (2023+)
- No subscription required
- Standard approach for retail traders

**Alternative: CME Group DataMine (Paid)**
- Authoritative source (direct from exchange)
- Requires subscription: $500-1000+/month for retail access
- URL: https://www.cmegroup.com/market-data/datamine-historical-data.html
- Overkill for daily high/low tracking (designed for institutional quant research)

**Not Recommended: Interactive Brokers**
- Limited to 1-2 years of historical daily data
- Requires IB account and active connection
- Good for real-time data, not historical analysis

## Installation

### 1. Install Python Dependencies

```bash
mamba activate mts
pip install yfinance pandas
```

**Why Yahoo Finance for MES Trading?**
- ES=F and MES are both CME products tracking S&P 500 index
- **Identical price levels** - only contract multiplier differs
  - MES: $5 per point (1/10th size)
  - ES: $50 per point (standard size)
- Yahoo Finance: Free, reliable, multi-year history
- CME DataMine: Paid subscription ($500-1000+/month), institutional focus
- Interactive Brokers: Limited to 1-2 years historical data
- This is the standard free approach used by retail MES traders globally

### 2. Verify Installation

```bash
python -c "import yfinance, pandas; print('✅ Dependencies installed')"
```

**Option 1: Interactive Brokers (Recommended - Actual MES Data)**
```bash
# Ensure IB Gateway/TWS is running on port 4002
# Fetch data from 2023 to present
python populate_daily_high_low_ib.py --start-date 2023-01-01

# Custom date range
python populate_daily_high_low_ib.py --start-date 2024-01-01 --end-date 2024-12-31

# Paper trading account (port 7497)
python populate_daily_high_low_ib.py --port 7497 --start-date 2023-01-01
```

**Option 2: Yahoo Finance (Fallback - ES=F Proxy)**
```bash
# Uses ES=F as proxy (identical price levels to MES)
python populate_daily_high_low.py --start-date 2023-01-01
python populate_daily_high_low.py --start-date 2024-01-01 --end-date 2024-12-31

# Specify output location
python populate_daily_high_low.py --output /path/to/daily_high_low.csv
```

### Daily Updates (Manual)

```bash
# Run end-of-day update script
./update_daily_data.sh
```

### Automated Updates (Cron Job)

Set up cron job to run at 5pm CT daily (after markets close):

```bash
# Edit crontab
crontab -e

# Add this line (adjust paths as needed):
# Run at 5:00 PM CT on weekdays (Mon-Fri)
0 17 * * 1-5 /home/rcruz/devel/VSCode/MindfulTrader/update_daily_data.sh >> /var/log/trading/daily_update.log 2>&1
```

**Note:** You may need to adjust the hour for your timezone:
- 5pm CT = 23:00 UTC (standard time)
- 5pm CT = 22:00 UTC (daylight saving time)

To create log directory:
```bash
sudo mkdir -p /var/log/trading
sudo chown $USER /var/log/trading
```

## File Formats

### daily_high_low.csv

```csv
date,high,low
2024-01-02,6125.50,6050.25
2024-01-03,6138.75,6105.00
2024-01-04,6152.25,6118.50
```
Primary Source:** Interactive Brokers (actual MES contract via ib_insync)  
**Fallback Source:** Yahoo Finance ES=F (E-mini S&P 500 as proxy)

**Note:** MES (Micro E-mini S&P 500) and ES (E-mini S&P 500) track the same S&P 500 index. The only difference is contract size ($5/point for MES vs $50/point for ES). Daily high/low **price levels are identical** - contract size doesn't affect the actual price. The IB version fetches actual MES contract data, while yfinance uses ES=F as a proxy (same levels)
**Source:** Yahoo Finance ES=F (E-mini S&P 500)
**CME products** tracking the same S&P 500 index:
- **MES**: Micro contract, $5 per index point, trades on CME Globex
- **ES**: Standard contract, $50 per index point, trades on CME Globex
- **Price Levels**: IDENTICAL (6125.50 on ES = 6125.50 on MES)
- **Only Difference**: Contract multiplier (affects P&L calculation, not price)

**Data Source Comparison:**
- **CME DataMine**: Authoritative but $500-1000+/month subscription
- **Yahoo Finance**: Free, reliable, same data (both ES and MES are CME products)
- **Interactive Brokers**: Only 1-2 years of historical daily bars

For daily high/low tracking, Yahoo Finance ES=F is the practical choice. CME DataMine is designed for institutional quantitative research with tick-level data, not simple daily OHLC bar
- **ES**: Standard contract, $50 per index point

The contract size affects P&L calculation, not the actual index price. A session high of 6125.50 on ES=F is exactly 6125.50 on MES. Yahoo Finance ES=F provides reliable historical data going back years, while Interactive Brokers typically only offers 1-2 years of daily data for futures contracts.

**Source:** Yahoo Finance ES=F (E-mini S&P 500)

**Note:** MES (Micro E-mini S&P 500) and ES (E-mini S&P 500) track the same S&P 500 index. The only difference is contract size ($5/point for MES vs $50/point for ES). Daily high/low **price levels are identical** - contract size doesn't affect the actual price. We use ES=F data because it has better liquidity and data availability on Yahoo Finance.

### nh_nl_historical.csv

```csv
date,nh_daily,nl_daily,nh_weekly,nl_weekly
2024-01-02,145,52,823,412
2024-01-03,189,38,901,385
```

**Fields:**
- `date`: Trading date in YYYY-MM-DD format
- `nh_daily`: NYSE new highs (daily)
- `nl_daily`: NYSE new lows (daily)
- `nh_weekly`: NYSE new highs (7-day rolling sum)
- `nl_weekly`: NYSE new lows (7-day rolling sum)

**Source:** Manual download from StockCharts.com (no API available)
- URL: https://stockcharts.com/freecharts/nh-nl.html
- See: [docs/ELDER_NH_NL_METHODOLOGY.md](docs/ELDER_NH_NL_METHODOLOGY.md)

## Data Location

### Linux Development
- **Local:** `data/daily_high_low.csv`
- **Local:** `data/nh_nl_historical.csv`

### Windows/Sierra Chart (via WSL)
- **Windows:** `C:/Trading/data/daily_high_low.csv`
- **Windows:** `C:/Trading/data/nh_nl_historical.csv`

The `update_daily_data.sh` script automatically copies data to Windows path if running on WSL.

## Troubleshooting

### yfinance Connection Issues

```bash
# Test connection
python -c "import yfinance as yf; data = yf.download('ES=F', start='2024-01-01', end='2024-01-02'); print(data)"
```

### Missing Data Days

Futures markets trade Sunday-Friday with different hours than stocks:
- ES futures: Sunday 6pm - Friday 5pm ET (with daily breaks)
- Some dates may have limited/no data due to holidays

### Cron Job Not Running

```bash
# Check cron status
systemctl status cron_ib.py (Recommended)

**Trading Instrument:** MES (Micro E-mini S&P 500)  
**Data Source:** Interactive Brokers API (actual MES contract)

**Features:**
- Connects to IB Gateway/TWS via ib_insync library
- Fetches actual MES futures contract data (not a proxy)
- Auto-qualifies front month contract
- Includes extended trading hours (24/5 futures market)
- Handles contract rollovers automatically

**Options:**
- `--start-date YYYY-MM-DD`: Start date (default: 2023-01-01)
- `--end-date YYYY-MM-DD`: End date (default: today)
- `--output PATH`: Output file path (default: data/daily_high_low.csv)
- `--host IP`: IB Gateway host (default: 127.0.0.1)
- `--port PORT`: IB Gateway port (4002 live, 7497 paper)
- `--overwrite`: Overwrite existing file instead of appending

**Requirements:**
- IB Gateway or TWS must be running
- API connections enabled in IB settings
- Valid IB account (live or paper)

### populate_daily_high_low.py (Fallback)

**Trading Instrument:** MES (Micro E-mini S&P 500)  
**Data Source:** Yahoo Finance ES=F (proxy with identical price levels)

**Features:**
- Fetches ES=F (E-mini S&P 500) data from Yahoo Finance
- No IB account or connection required
bash -x ./update_daily_data.sh
```

## Script Details

### populate_daily_high_low.py

**Trading Instrument:** MES (Micro E-mini S&P 500)

**Features:**
- Fetches ES=F (E-mini S&P 500) data from Yahoo Finance as data source
- MES and ES have identical price levels (same underlying index)
- Extracts daily high/low values
- Writes to CSV format compatible with DailyHighLowLoader
- Supports date range specification
- Appends new data without overwriting existing

**Options:**
- `--ticker SYMBOL`: Yahoo Finance ticker (default: ES=F)
- `--overwrite`: Overwrite existing file instead of appending

**Why ES=F for MES Trading:**
MES (Micro) and ES (Standard) are both S&P 500 futures. They differ only in contract multiplier:
- MES: $5 per index point (1/10th the size)
- ES: $50 per index point

The actual **index price is identical** for both contracts. A high of 6125.50 on ES=F is the same 6125.50 level for MES. Contract size affects P&L calculation, not the price levels themselves.
- `--end-date YYYY-MM-DD`: End date (default: today)
- `--output PATH`: Output file path (default: data/daily_high_low.csv)
- `--overwrite`: Overwrite existing file instead of appending

### update_daily_data.sh

**Features:**
- Activates mamba 'mts' environment
- Updates daily_high_low.csv with yesterday's data
- Logs execution to file
- Copies data to Windows path if running on WSL
- Exit on error (safe for cron)

**Cron-friendly:**
- Logs with timestamps
- Exits with proper status codes
- Handles mamba environment activation

## Integration with C++ Studies

Data is loaded automatically by Sierra Chart studies:

**DailyHighLowLoader** (`src/DailyHighLowLoader.cpp`):
- Singleton pattern with auto-load
- Reads: `C:/Trading/data/daily_high_low.csv`
- 1-day lag: Uses yesterday's data for today's bars
- Horizontal lines drawn at previous session high/low

**NhNlDataLoader** (`src/NhNlDataLoader.cpp`):
- Singleton pattern with auto-load
- Reads: `C:/Trading/data/nh_nl_historical.csv`
- Provides NH-NL breadth signal for regime detection
- Used by CalculateNhNlSignal() in StudyHelperFunctions.cpp

## Maintenance Schedule

**Daily (Automated via Cron):**
- 5pm CT: Update daily_high_low.csv

**Weekly (Manual):**
- Download NH-NL data from StockCharts.com
- Update nh_nl_historical.csv

**Monthly:**
- Verify data quality (no gaps, reasonable values)
- Check log files for errors
- Validate horizontal lines display correctly in Sierra Chart

## See Also

- [docs/DATA_EXPORT_WORKFLOW.md](docs/DATA_EXPORT_WORKFLOW.md) - Overall data pipeline
- [docs/ELDER_NH_NL_METHODOLOGY.md](docs/ELDER_NH_NL_METHODOLOGY.md) - NH-NL calculation details
- [docs/HEDGE_FUND_GAP_ANALYSIS.md](docs/HEDGE_FUND_GAP_ANALYSIS.md) - System documentation
- [include/DailyHighLowLoader.h](include/DailyHighLowLoader.h) - C++ implementation
- [include/NhNlDataLoader.h](include/NhNlDataLoader.h) - NH-NL loader implementation
