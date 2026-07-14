#!/usr/bin/env python3
"""
Populate daily_high_low.csv with MES futures data from Interactive Brokers

Usage:
    mamba activate mts
    # Ensure TWS or IB Gateway is running on port 4002
    python populate_daily_high_low_ib.py [--start-date YYYY-MM-DD] [--end-date YYYY-MM-DD]

Requirements:
    pip install ib_insync pandas

Prerequisites:
    - TWS (Trader Workstation) or IB Gateway must be running
    - API connections enabled in TWS/Gateway settings
    - Port 4002 for live trading, 7497 for paper trading
"""

import argparse
import sys
from datetime import datetime, timedelta
from pathlib import Path

try:
    from ib_insync import IB, Future, util
    import pandas as pd
except ImportError as e:
    print(f"Error: Missing required package - {e}")
    print("Install with: pip install ib_insync pandas")
    sys.exit(1)


def connect_ib(host: str = '127.0.0.1', port: int = 4002, client_id: int = 100) -> IB:
    """
    Connect to Interactive Brokers TWS/Gateway
    
    Args:
        host: IB Gateway host (default: localhost)
        port: IB Gateway port (4002 for live, 7497 for paper)
        client_id: Unique client ID
    
    Returns:
        Connected IB instance
    """
    ib = IB()
    
    print(f"Connecting to IB Gateway at {host}:{port}...")
    
    try:
        ib.connect(host, port, clientId=client_id, timeout=10)
        print(f"✅ Connected to IB (Client ID: {client_id})")
        return ib
    except Exception as e:
        print(f"❌ Connection failed: {e}")
        print("\nTroubleshooting:")
        print("1. Ensure TWS or IB Gateway is running")
        print("2. Enable API connections: Configure → Settings → API → Settings")
        print("3. Verify port number (4002 for live, 7497 for paper)")
        print("4. Add 127.0.0.1 to trusted IPs if needed")
        sys.exit(1)


def fetch_mes_daily_data(ib: IB, start_date: str, end_date: str, exchange: str = 'CME') -> pd.DataFrame:
    """
    Fetch MES futures daily high/low data from Interactive Brokers
    
    Args:
        ib: Connected IB instance
        start_date: Start date in YYYY-MM-DD format
        end_date: End date in YYYY-MM-DD format
        exchange: Exchange (default: CME)
    
    Returns:
        DataFrame with columns: date, high, low
    """
    # MES continuous contract specification
    # We'll use the front month contract
    contract = Future(
        symbol='MES',
        exchange=exchange,
        currency='USD'
    )
    
    print(f"\nResolving MES contract details...")
    
    try:
        # Qualify the contract to get the front month
        contracts = ib.qualifyContracts(contract)
        
        if not contracts:
            print("❌ Could not find MES contract")
            return pd.DataFrame()
        
        # Use the first qualified contract (front month)
        qualified = contracts[0]
        print(f"✅ Found contract: {qualified.localSymbol} ({qualified.lastTradeDateOrContractMonth})")
        
    except Exception as e:
        print(f"❌ Contract qualification failed: {e}")
        return pd.DataFrame()
    
    # Calculate duration for historical data request
    start_dt = datetime.strptime(start_date, '%Y-%m-%d')
    end_dt = datetime.strptime(end_date, '%Y-%m-%d')
    duration_days = (end_dt - start_dt).days
    
    # IB requires specific duration strings
    if duration_days <= 30:
        duration_str = f"{duration_days} D"
    elif duration_days <= 365:
        duration_str = f"{int(duration_days/7)} W"
    else:
        duration_str = f"{int(duration_days/365)} Y"
    
    print(f"\nFetching historical data...")
    print(f"Date range: {start_date} to {end_date} ({duration_days} days)")
    print(f"Duration string: {duration_str}")
    
    try:
        # Request historical daily bars
        bars = ib.reqHistoricalData(
            qualified,
            endDateTime=end_date,
            durationStr=duration_str,
            barSizeSetting='1 day',
            whatToShow='TRADES',
            useRTH=False,  # Include extended trading hours for futures
            formatDate=1
        )
        
        if not bars:
            print("❌ No data returned")
            return pd.DataFrame()
        
        print(f"✅ Retrieved {len(bars)} trading days")
        
        # Convert to DataFrame
        df = util.df(bars)
        
        # Extract and format data
        result = pd.DataFrame({
            'date': pd.to_datetime(df['date']).dt.strftime('%Y-%m-%d'),
            'high': df['high'].round(2),
            'low': df['low'].round(2)
        })
        
        # Filter to exact date range
        result['date_dt'] = pd.to_datetime(result['date'])
        start_filter = pd.to_datetime(start_date)
        end_filter = pd.to_datetime(end_date)
        result = result[(result['date_dt'] >= start_filter) & (result['date_dt'] <= end_filter)]
        result = result.drop('date_dt', axis=1)
        
        return result
        
    except Exception as e:
        print(f"❌ Historical data request failed: {e}")
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
        description='Populate daily_high_low.csv with MES futures data from Interactive Brokers'
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
        '--host',
        type=str,
        default='127.0.0.1',
        help='IB Gateway host (default: 127.0.0.1)'
    )
    parser.add_argument(
        '--port',
        type=int,
        default=4002,
        help='IB Gateway port (default: 4002 for live, 7497 for paper)'
    )
    
    args = parser.parse_args()
    
    print("=" * 70)
    print("MES Micro E-mini S&P 500 Daily High/Low Data Fetch")
    print("Data Source: Interactive Brokers (Actual MES Contract)")
    print("=" * 70)
    
    # Validate dates
    try:
        datetime.strptime(args.start_date, '%Y-%m-%d')
        datetime.strptime(args.end_date, '%Y-%m-%d')
    except ValueError:
        print("❌ Error: Dates must be in YYYY-MM-DD format")
        sys.exit(1)
    
    # Connect to IB
    ib = connect_ib(host=args.host, port=args.port)
    
    try:
        # Fetch data
        df = fetch_mes_daily_data(ib, args.start_date, args.end_date)
        
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
        
    finally:
        # Disconnect
        ib.disconnect()
        print("\n✅ Disconnected from IB")


if __name__ == '__main__':
    main()
