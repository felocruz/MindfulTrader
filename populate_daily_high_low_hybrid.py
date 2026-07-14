#!/usr/bin/env python3
"""
Populate daily_high_low.csv with MES futures data

Data Source Options (in order of recommendation):
    1. Yahoo Finance ES=F - Free, reliable, multi-year history
       • ES and MES have identical prices (same S&P 500 index)
       • Only contract multiplier differs ($50/pt vs $5/pt)
    
    2. CME Group DataMine - Authoritative but requires paid subscription
       • https://www.cmegroup.com/market-data/datamine-historical-data.html
       • Direct from exchange, but $500-1000+/month for retail
    
    3. Interactive Brokers - Limited to 1-2 years historical
       • Good for recent data verification
       • Not suitable for multi-year backtesting

This script uses Yahoo Finance (free, practical for retail traders).

Usage:
    mamba activate mts
    python populate_daily_high_low_hybrid.py [--start-date YYYY-MM-DD] [--end-date YYYY-MM-DD]

Requirements:
    pip install yfinance pandas
"""

import argparse
import sys
from datetime import datetime, timedelta
from pathlib import Path

try:
    import yfinance as yf
    import pandas as pd
except ImportError as e:
    print(f"Error: Missing required package - {e}")
    print("Install with: pip install yfinance pandas")
    sys.exit(1)


def fetch_es_daily_data(start_date: str, end_date: str) -> pd.DataFrame:
    """
    Fetch ES futures daily high/low data from Yahoo Finance
    
    ES (E-mini S&P 500) and MES (Micro E-mini S&P 500) have identical price levels.
    Only difference is contract multiplier ($50/pt vs $5/pt).
    
    Args:
        start_date: Start date in YYYY-MM-DD format
        end_date: End date in YYYY-MM-DD format
    
    Returns:
        DataFrame with columns: date, high, low
    """
    ticker = "ES=F"
    
    print(f"Fetching {ticker} data from {start_date} to {end_date}...")
    print(f"Note: ES and MES have identical prices (same S&P 500 index)")
    print(f"      Only contract size differs ($50/pt vs $5/pt for P&L)")
    
    try:
        # Download with auto_adjust to suppress warnings
        data = yf.download(
            ticker, 
            start=start_date, 
            end=end_date, 
            progress=False,
            auto_adjust=True
        )
        
        if data.empty:
            print(f"Warning: No data returned for {ticker}")
            return pd.DataFrame()
        
        # Handle potential multi-index columns
        if isinstance(data.columns, pd.MultiIndex):
            data.columns = data.columns.get_level_values(0)
        
        # Extract date, high, low
        df = pd.DataFrame({
            'date': data.index.strftime('%Y-%m-%d'),
            'high': pd.Series(data['High']).round(2),
            'low': pd.Series(data['Low']).round(2)
        })
        
        # Remove any rows with NaN values
        df = df.dropna()
        
        print(f"✅ Retrieved {len(df)} trading days from Yahoo Finance")
        return df
        
    except Exception as e:
        print(f"❌ Error fetching data: {e}")
        import traceback
        traceback.print_exc()
        return pd.DataFrame()


def write_csv(df: pd.DataFrame, output_path: Path, overwrite: bool = False):
    """
    Write DataFrame to CSV file
    
    Args:
        df: DataFrame with columns: date, high, low
        output_path: Path to output CSV file
        overwrite: If True, overwrite existing file; if False, append new data
    """
    if df.empty:
        print("No data to write")
        return
    
    # Create parent directory if it doesn't exist
    output_path.parent.mkdir(parents=True, exist_ok=True)
    
    if overwrite or not output_path.exists():
        # Write new file with header
        df.to_csv(output_path, index=False, float_format='%.2f')
        print(f"\n✅ Wrote {len(df)} rows to {output_path}")
    else:
        # Load existing data
        existing_df = pd.read_csv(output_path)
        existing_dates = set(existing_df['date'])
        
        # Filter out dates that already exist
        new_df = df[~df['date'].isin(existing_dates)]
        
        if new_df.empty:
            print("\nℹ️  No new data to append (all dates already exist)")
            return
        
        # Append new data
        combined_df = pd.concat([existing_df, new_df], ignore_index=True)
        
        # Sort by date
        combined_df['date'] = pd.to_datetime(combined_df['date'])
        combined_df = combined_df.sort_values('date')
        combined_df['date'] = combined_df['date'].dt.strftime('%Y-%m-%d')
        
        # Write combined data
        combined_df.to_csv(output_path, index=False, float_format='%.2f')
        print(f"\n✅ Appended {len(new_df)} new rows to {output_path}")
        print(f"Total rows: {len(combined_df)}")


def main():
    parser = argparse.ArgumentParser(
        description='Populate daily_high_low.csv for MES trading (uses ES=F data)',
        epilog='Note: ES and MES have identical price levels (same underlying S&P 500 index)'
    )
    parser.add_argument(
        '--start-date',
        type=str,
        default='2023-01-01',
        help='Start date in YYYY-MM-DD format (default: 2023-01-01)'
    )
    parser.add_argument(
        '--end-date',
        type=str,
        default=datetime.now().strftime('%Y-%m-%d'),
        help='End date in YYYY-MM-DD format (default: today)'
    )
    parser.add_argument(
        '--output',
        type=str,
        default='data/daily_high_low.csv',
        help='Output CSV file path (default: data/daily_high_low.csv)'
    )
    parser.add_argument(
        '--overwrite',
        action='store_true',
        help='Overwrite existing file instead of appending'
    )
    
    args = parser.parse_args()
    
    print("=" * 70)
    print("MES (Micro E-mini S&P 500) Daily High/Low Data")
    print("Data Source: Yahoo Finance ES=F (E-mini S&P 500)")
    print("=" * 70)
    print()
    print("📊 Trading Context:")
    print("   • You trade: MES (CME Micro E-mini S&P 500) - $5 per point")
    print("   • Data source: ES=F (CME E-mini S&P 500) - $50 per point")
    print("   • Exchange: Both trade on CME Globex")
    print("   • Price levels: IDENTICAL (both track S&P 500 index)")
    print("   • Difference: Contract multiplier only (affects P&L, not price)")
    print()
    print("✅ Why Yahoo Finance instead of CME direct:")
    print("   • CME DataMine requires paid subscription ($500-1000+/month)")
    print("   • ES=F data is free and has multi-year history")
    print("   • Price levels are identical (CME products, same index)")
    print("   • Standard approach for retail MES traders")
    print("=" * 70)
    print()
    
    # Validate dates
    try:
        datetime.strptime(args.start_date, '%Y-%m-%d')
        datetime.strptime(args.end_date, '%Y-%m-%d')
    except ValueError:
        print("❌ Error: Dates must be in YYYY-MM-DD format")
        sys.exit(1)
    
    # Fetch data
    df = fetch_es_daily_data(args.start_date, args.end_date)
    
    if df.empty:
        print("\n❌ Failed to fetch data")
        sys.exit(1)
    
    # Show sample
    print("\n" + "=" * 70)
    print("Sample data (first 5 rows):")
    print(df.head().to_string(index=False))
    print("\nSample data (last 5 rows):")
    print(df.tail().to_string(index=False))
    print("=" * 70)
    
    # Write to file
    output_path = Path(args.output)
    write_csv(df, output_path, overwrite=args.overwrite)
    
    print("\n" + "=" * 70)
    print(f"✅ Success! Data written to {output_path}")
    print(f"Date range: {df['date'].min()} to {df['date'].max()}")
    print(f"Trading days: {len(df)}")
    print("=" * 70)
    print()
    print("📝 Next Steps:")
    print("   1. Review sample data above for accuracy")
    print("   2. Copy to Windows: cp data/daily_high_low.csv /mnt/c/Trading/data/")
    print("   3. Restart Sierra Chart studies to load new data")
    print("   4. Verify horizontal lines display correctly on chart")
    print()


if __name__ == '__main__':
    main()
