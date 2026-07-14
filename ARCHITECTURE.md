# MindfulTrader Architecture

## Overview

This document outlines the architecture of the MindfulTrader system, a real-time trading application developed as an Advance Custom Study for the Sierra Chart platform.

## Core Strategy

The trading strategy is built upon two primary sources:

1.  **Alexander Elder's Triple Screen System:** This provides the foundational framework for market analysis across multiple timeframes.
2.  **Linda Raschke's Patterns and Setups:** Specific trading patterns and entry/exit setups are derived from the methodologies of Linda Raschke.

## System Components

The system is designed in a modular way, with distinct components responsible for different aspects of the trading logic and communication.

### 1. Main Orchestrator (`scsf_MindfulTrader` in `SCStudies.cpp`)

This is the central entry point and orchestrator of the entire trading system within the Sierra Chart environment. Its primary responsibilities include:

-   **Initialization:** Sets up core managers (`IndicatorManager`, `PositionManager`) and runtime messaging services (`TransportStream`, `SystemOrchestrator`, `AIHeartbeatMonitor`) when the study is loaded.
-   **Main Loop:** On each chart update, it coordinates the updates for all other components. It calls `PositionManager::Update()` and `IndicatorManager::UpdateBarContext()` to process the latest market data.
-   **GUI Connection Management:** Handles the logic for connecting and disconnecting from the external Python GUI, including managing the menu items in Sierra Chart.
-   **Data Publishing:** Publishes indicator and position data to the GUI when connected.

### 2. Indicator Management

-   **`IndicatorManager` (`IndicatorManager.cpp`, `IndicatorManager.h`):** A singleton class that manages all trading indicators.
    -   It holds a collection of `Indicator` objects, each representing a specific market indicator (e.g., MACD, Impulse, Stochastic).
    -   It is responsible for updating all indicators based on the latest chart data and caching the results in a JSON payload.
    -   It provides methods to get the current state of any indicator.

-   **`Indicator` (`Indicator.cpp`, `Indicator.h`):** A base class with derived classes for each specific indicator type.
    -   Each `Indicator` class encapsulates the logic for calculating its value and determining if its state has changed (`IsDirty`).
    -   This design allows for a clean separation of concerns, where each indicator's logic is self-contained.

### 3. Position Management

-   **`PositionManager` (`PositionManager.cpp`, `PositionManager.h`):** A singleton class responsible for managing the current trading position.
    -   It tracks the state of the current trade (e.g., open, closed, flat).
    -   It handles order fills from Sierra Chart to open, close, or scale into positions.
    -   It communicates with the `TradeSocketZMQ` to send trade data to the backend for storage and analysis.
    -   It publishes the current position status to the GUI.

### 4. Triple Screen Studies (`TripleScreen1.cpp`, `TripleScreen2.cpp`, `TripleScreen3.cpp`)

These files contain the implementation of the individual Sierra Chart studies that make up the Triple Screen system. Each file corresponds to one "screen" of the system:

-   **`TripleScreen1.cpp`:** Long-term timeframe analysis (e.g., weekly chart).
-   **`TripleScreen2.cpp`:** Intermediate timeframe analysis (e.g., daily chart), where trade setups are identified.
-   **`TripleScreen3.cpp`:** Short-term timeframe analysis (e.g., intraday chart), used for precise entry and exit timing.

Each of these studies calculates its respective indicators and updates the shared `IndicatorManager` with the results.

### 5. ZeroMQ Communication Layer

The system uses ZeroMQ for real-time, asynchronous communication with an external Python-based GUI and backend. This decouples the C++ trading logic from the user interface and data persistence layers.

-   **`transport/TransportStream.cpp` / `.h`:** Manages the primary `PUB` (publish) stream for indicators/events to downstream consumers.

-   **`TradeExecutionServer.cpp` / `.h`:** Handles trade-related request/response flows for validation and execution on the C++ runtime side.

-   **`SystemOrchestrator.cpp` / `.h`:** Owns control-plane handshake and runtime negotiation (CONFIG_REQ/CONFIG_ACK), including endpoint negotiation for dependent services.

### 6. Implemented Indicators

The following is a list of the indicators currently implemented and managed by the `IndicatorManager`, categorized by their primary Triple Screen timeframe.

#### Screen 1: Long-Term Trend

-   **MACD (`LONG_MACD`):** Moving Average Convergence Divergence for trend direction.
-   **13-period Force Index (`LONG_FI13_SIGNAL`):** Measures the strength of bull and bear power.
-   **Impulse System (`LONG_IMP`):** Combines an EMA and MACD to identify trend inertia.
-   **EMA (`LONG_EMA`):** Exponential Moving Average.
-   **Market Action (`LONG_MKT_ACTION`):** Tracks price in relation to value zones.

#### Screen 2: Intermediate Setups

-   **Stochastic (`INTERM_STOCHASTIC`):** Oscillator for identifying overbought/oversold conditions and divergences.
-   **Raschke Setups (`RASCHKE_SETUP`):** Detects various price patterns defined by Linda Raschke (e.g., Holy Grail, Pinball, Anti).
-   **RSI (`RSI`):** Relative Strength Index.
-   **2-period Force Index (`INTERM_FI2_SIGNAL`):** A short-term version of the Force Index used to find entry points.
-   **EMA Proximity (`EMA_PROXIMITY`):** Measures how close the price is to its EMA.
-   **Price Metrics (`PRICE_METRICS`):** Analyzes OHLC data to determine bar strength.
-   **Impulse System (`INTERM_IMP`):** The intermediate timeframe version of the Impulse System.
-   **MACD (`INTERM_MACD`):** The intermediate timeframe version of the MACD.
-   **Market Action (`INTERM_MKT_ACTION`):** The intermediate timeframe version of Market Action.

#### Screen 3: Short-Term Timing

-   **Structure Test (`STRUCTURE_TEST`):** Analyzes how the current price is interacting with previous highs and lows.
-   **Volume (`VOLUME`):** Tracks volume levels and spikes.
-   **ATR Proximity (`ATR_PROXIMITY`):** Measures the bar's range relative to the Average True Range (ATR).
-   **Market Action (`SHORT_MKT_ACTION`):** The short-term timeframe version of Market Action.

#### General

-   **Trade Side (`SIDE`):** Indicates the current position (long, short, or flat).
-   **Market Symbol (`MARKET_SYMBOL`):** The symbol of the instrument being traded.

---

### March 2026 Architectural Changes

**ADX Retirement & Hurst Migration**: ADX computation (`sc.ADX()` / `sc.InternalADX()`) has been removed from all three TripleScreen studies. The **Hurst exponent** (DFA-based persistence measurement) is now the sole trend persistence metric for pattern detection gates (Holy Grail, Elder Breakout, Turtle Soup). Subgraph slots are preserved as `DRAWSTYLE_IGNORE` for Sierra Chart settings stability. See [docs/TRADE_EXECUTION_SYSTEM.md](docs/TRADE_EXECUTION_SYSTEM.md) and [docs/TRADE_EXECUTION_MESSAGE_PROTOCOL.md](docs/TRADE_EXECUTION_MESSAGE_PROTOCOL.md) for the current source-of-truth contract.

**ATR Consolidation**: ATR computation standardized to Wilder's smoothing throughout. TripleScreen1 (ATR-14) and TripleScreen3 (ATR-10) now push cached values to `PositionManager`, eliminating redundant per-tick True Range loops. Pattern stops are DOF-scaled via `sqrt((dof+1)/(dof-1))`.

This document will be updated as the system's architecture evolves.
