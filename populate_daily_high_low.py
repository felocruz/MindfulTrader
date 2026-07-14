#!/usr/bin/env python3
"""
Populate daily_high_low.csv with historical MES futures data

Usage:
    mamba activate mts
    python populate_daily_high_low.py [--start-date YYYY-MM-DD] [--end-date YYYY-MM-DD]

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


def fetch_mes_daily_data(start_date: str, end_date: str, ticker: str = "ES=F") -> pd.DataFrame:
    """
    Fetch MES futures daily high/low data from Yahoo Finance
    
    Args:
        start_date: Start date in YYYY-MM-DD format
        end_date: End date in YYYY-MM-DD format
        ticker: Yahoo Finance ticker symbol (default: ES=F)
    
    Returns:
        DataFrame with columns: date, high, low
    
    Note:
        MES (Micro E-mini S&P 500) and ES (E-mini S&P 500) track the same index.
        The only difference is contract size ($5/point vs $50/point).
        Daily high/low price levels are identical - contract size doesn't affect price.
        
        Yahoo Finance tickers:
        - ES=F: E-mini S&P 500 Futures (most liquid, best data availability)
        - MES may not be available on Yahoo Finance
    """
    print(f"Fetching {ticker} data from {start_date} to {end_date}...")
    print(f"Note: Using {ticker} data for MES trading (price levels are identical)")
    
    try:
        # Download data with auto_adjust to avoid FutureWarning
        data = yf.download(ticker, start=start_date, end=end_date, progress=False, auto_adjust=True)
        
        if data.empty:
            print(f"Warning: No data returned for {ticker}")
            return pd.DataFrame()
        
        # Handle multi-index columns from yfinance
        if isinstance(data.columns, pd.MultiIndex):
            # Flatten multi-index columns (ticker, field) -> field
            data.columns = data.columns.get_level_values(0)
        
        # Extract date, high, low
        df = pd.DataFrame({
            'date': data.index.strftime('%Y-%m-%d'),
            'high': data['High'].values.flatten(),
            'low': data['Low'].values.flatten()
        })
        
        # Round to 2 decimal places
        df['high'] = df['high'].round(2)
        df['low'] = df['low'].round(2)
        
        # Remove any rows with NaN values
        df = df.dropna()
        
        print(f"Retrieved {len(df)} trading days")
        return df
        
    except Exception as e:
        print(f"Error fetching data: {e}")
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
        print(f"Wrote {len(df)} rows to {output_path}")
    else:
        # Load existing data
        existing_df = pd.read_csv(output_path)
        existing_dates = set(existing_df['date'])
        
        # Filter out dates that already exist
        new_df = df[~df['date'].isin(existing_dates)]
        
        if new_df.empty:
            print("No new data to append (all dates already exist)")
            return
        
        # Append new data
        combined_df = pd.concat([existing_df, new_df], ignore_index=True)
        
        # Sort by date
        combined_df['date'] = pd.to_datetime(combined_df['date'])
        combined_df = combined_df.sort_values('date')
        combined_df['date'] = combined_df['date'].dt.strftime('%Y-%m-%d')
        
        # Write combined data
        combined_df.to_csv(output_path, index=False, float_format='%.2f')
        print(f"Appended {len(new_df)} new rows to {output_path}")
        print(f"Total rows: {len(combined_df)}")


def main():
    parser = argparse.ArgumentParser(
        description='Populate daily_high_low.csv with MES futures data'
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
    parser.add_argument(
        '--ticker',
        type=str,
        default='ES=F',
        help='Yahoo Finance ticker symbol (default: ES=F for E-mini S&P 500)'
    )
    
    args = parser.parse_args()
    
    print("=" * 60)
    print("MES Micro E-mini S&P 500 Daily High/Low Data Fetch")
    print("=" * 60)
    print(f"Trading Instrument: MES (Micro E-mini S&P 500)")
    print(f"Data Source: {args.ticker} via Yahoo Finance")
    print(f"Note: MES and ES track the same index with identical price levels")
    print(f"      Only difference is contract size ($5/point vs $50/point)")
    print("=" * 60)
    print()
    
    # Validate dates
    try:
        datetime.strptime(args.start_date, '%Y-%m-%d')
        datetime.strptime(args.end_date, '%Y-%m-%d')
    except ValueError:
        print("Error: Dates must be in YYYY-MM-DD format")
        sys.exit(1)
    
    # Fetch data
    df = fetch_mes_daily_data(args.start_date, args.end_date)
    
    if df.empty:
        print("Failed to fetch data")
        sys.exit(1)
    
    # Show sample
    print("\nSample data (first 5 rows):")
    print(df.head())
    print("\nSample data (last 5 rows):")
    print(df.tail())
    
    # Write to file
    output_path = Path(args.output)
    write_csv(df, output_path, overwrite=args.overwrite)
    
    print(f"\n✅ Success! Data written to {output_path}")
    print(f"Date range: {df['date'].min()} to {df['date'].max()}")


if __name__ == '__main__':
    main()
