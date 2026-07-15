# Liquidity and Toxicity Gate Decision

**To:** Quantitative Research & Trading Systems Engineering
**From:** Elite Microstructure Research
**Subject:** Ruling on Pre-Trade Veto Signal (Amihud vs. VPIN)
**Date:** July 14, 2026

## 1. The VPIN Critique and Redundancy
The Andersen & Bondarenko (2014) critique of VPIN is devastating and entirely applicable to this system. A&B demonstrated that VPIN’s predictive power for toxicity and crashes is subsumed by standard, simpler metrics (like trading volume and return volatility). VPIN relies on Bulk Volume Classification (BVC) to approximate trade direction when only aggregated bar data is available. Because BVC introduces significant estimation artifacts, VPIN is fundamentally a noisy heuristic. 

In this system, you already possess direct Order Flow Imbalance (OFI) and Time-&-Sales (T&S) micro-asymmetry. Therefore, implementing VPIN would mean introducing a mathematically flawed, artifact-heavy proxy to measure something you are already measuring directly and accurately. VPIN adds **zero** decision value here; it is redundant and empirically inferior to your existing signals.

## 2. Elite-Fund Standard for Pre-Trade Vetoes
At the 15-minute frequency on a highly liquid instrument like ES futures, elite institutional systematic funds separate the pre-trade veto into two distinct but complementary risk axes:
1. **Illiquidity (Price Impact):** How much will the market move against me if I cross the spread? (Amihud 2002; Kyle 1985). 
2. **Toxicity (Adverse Selection):** Is there informed, aggressive directional flow sweeping the book right now? (Easley et al. 1996 for the theoretical basis of PIN, modernized by Cont et al. 2014 for OFI).

The institutional standard is not a single blended metric, but a dual-axis check using direct measurements: Amihud (or Kyle's Lambda) for price impact, and direct OFI/T&S aggressor pressure for toxicity.

## 3. Threshold Calibration Warning
Comparing a raw Amihud calculation to a fixed `0.80` threshold is a critical architectural flaw. Raw Amihud is unbounded, non-stationary, and highly sensitive to underlying price levels and volume regimes (a structural break in ES volume renders fixed thresholds obsolete). 

To use Amihud correctly, it must be mapped to a stationary distribution. The standard approach is to use a **rolling empirical CDF (percentiles)** or a **robust Z-score** (using median and MAD) over a trailing window (e.g., 21 trading days). The regime-aware tightening logic (dropping the threshold from normal to fat-tail regimes) should operate on these percentiles (e.g., veto if illiquidity > 90th percentile normally, tightened to > 75th percentile in high-kurtosis/low-DOF regimes).

## 4. Recommended Design (The Combination)
I recommend a modified **Option 4: A Dual-Axis Regime-Normalized Veto**.

Do not implement VPIN. Keep the current Amihud calculation, rename it honestly to `Amihud_Illiquidity`, and normalize it to a rolling percentile. Combine this with your existing OFI/T&S asymmetry (also normalized) using a **Logical OR** for the veto.

**Veto Logic:**
`REJECT IF (Amihud_Illiquidity_Percentile > Regime_Threshold_L) OR (OFI_Toxicity_Percentile > Regime_Threshold_T)`

*Why Logical OR?* Because either risk axis is fatal on its own. You do not want to enter a trade if the book is empty (high Amihud), even if flow is balanced. Conversely, you do not want to enter a trade if a toxic institutional sweep is occurring (high OFI), even if the book appears deep. 

## 5. Final Decisive Recommendation
**Reject VPIN completely, retain and honestly rename Amihud as your price-impact proxy, and normalize it via rolling empirical percentiles to form a Logical OR dual-veto alongside your existing Order Flow Imbalance signal.** 

*Why it beats alternatives:* It abandons the mathematically flawed and redundant VPIN proxy in favor of direct, institutional-standard measurements of both illiquidity (Amihud) and toxicity (OFI), while fixing the critical stationarity bug in the fixed-threshold comparison.