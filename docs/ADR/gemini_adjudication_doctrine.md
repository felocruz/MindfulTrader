# Gemini Adjudication Doctrine: Triple-Barrier Execution & Parity

**To:** Quantitative Research & Trading Systems Engineering (Claude Code & Peers)  
**From:** Gemini CLI (Adjudicator)  
**Date:** July 14, 2026  
**Subject:** Sovereign Adjudication on Triple-Barrier Parity, Runner Alpha, and Structural Execution Controls  

---

## Executive Summary

This document serves as the formal quantitative adjudication on the structural divergence between the supervised machine learning labeler (`lbrnet`) and the live execution engine (`MindfulTrader` C++). 

Our ruling is absolute: **The live execution engine's historical multi-exit scale-out ladder, trailing stop, and "uncapped runner" mechanism are structurally in error. Option A (Single-Stage Immutable First-Touch Bracket) must govern the Phase 1 cutover.** 

Any divergence between training-label outcomes and live execution paths introduces an unhedgeable, mathematically ungrounded mismatch that invalidates supervised learning assumptions. "Letting winners run" is a valid trading heuristic, but it must be moved to an independent, separately grounded layer of the portfolio architecture rather than being grafted onto a single-target first-touch label.

---

## 1. Train/Live Parity & Divergence Conditions

### The Fundamental Theorem of Supervised Labels
In supervised path-dependent classification, a model estimates the conditional probability of a specific, bounded path outcome: 
$$E[Y \mid X]$$
where $Y \in \{-1, 0, 1\}$ is defined by the triple-barrier intersection. 

If the live execution uses a multi-exit scale-out ladder (e.g., T1 50% / T2 30% / Runner 20%) and a server-side trailing stop, the realized out-of-sample $P\&L$ distribution and path trajectory will diverge catastrophically from the labeled training outcomes. You are training the model to predict whether a *static bracket* is hit first, but executing a *dynamic path modification*. 

For example, if the model identifies a high-probability Raschke setup and predicts it will hit a static $1.5R$ target with 90% confidence, but live execution exits 50% at $1.5R$, trails the remainder, and gets stopped out at breakeven on a subsequent pullback, the realized expectancy collapses relative to the model's calibrated prediction.

### Mathematically Sound Conditions for Divergence
Divergence between labeled outcomes and executed exits is theoretically defensible under exactly **one** condition: **Meta-Labeling (López de Prado, AFML Ch. 3.6)**.

If a primary model predicts the direction $Y_1 \in \{-1, 1\}$ under a strict single-stage first-touch bracket, a secondary model (the meta-labeler) can be trained on the binary success $Y_2 \in \{0, 1\}$ of a *complex live execution path* (including scale-outs and trailing stops). Because the meta-labeler's training features and labels are derived from the *actual realized P&L* of the complex execution path, train/live parity is mathematically preserved at the portfolio decision layer. 

Without this secondary model, however, executing anything other than the exact single-stage first-touch bracket used in training violates the IID assumptions of out-of-sample validation.

---

## 2. Where "Let Winners Run" Alpha Correctly Belongs

For a single-instrument, one-position-at-a-time futures system (ES), we rank the mathematically rigorous ways to capture trend-runner alpha within the López de Prado framework:

### Rank 1: Trend-Scanning Labels (López de Prado, MLAM Ch. 5) — *Winner*
* **Mechanism:** Rather than pre-defining static horizontal barriers at entry, fit a forward linear regression to the price path. The label is the t-value of the maximum regression length that maintains statistical significance.
* **Why it wins:** It natively identifies the trend's duration and strength. It completely replaces the triple-barrier with an adaptive, trend-following exit that is mathematically robust, eliminating the need for an arbitrary "uncapped runner" heuristic.

### Rank 2: Dynamic Regime-Scale Multipliers (AFML Ch. 3.4)
* **Mechanism:** Retain the single-stage first-touch bracket, but dynamically scale `target_r_mult` at entry using continuous indicators (e.g., Shannon entropy, Hurst exponent, or Student-t HMM state).
* **Why it ranks high:** In a highly persistent regime (Hurst $> 0.65$, `PARETO_MOMENTUM` HMM state), the profit-take barrier is expanded to $3.0R$ or $4.0R$ at entry. In a mean-reverting regime (`COILED_SPRING`), it is tightened to $1.0R$. Parity is preserved because the entry-time scaling parameters are identical in both training and live engines.

### Rank 3: Meta-Labeling → Bet Sizing (AFML Ch. 10)
* **Mechanism:** Predict the probability of a structural trend continuation using a meta-labeling model, and scale the position size (using a sigmoid bet-sizing function) rather than altering the exit geometry.
* **Why it ranks lower:** While mathematically elegant, it scales the risk units at entry but does not solve the exit-path divergence of a single position.

---

## 3. Literature-Grounded Improvements on the Triple-Barrier

| Method | Literature Source | Practical Tradeoff for Train/Live Parity |
| :--- | :--- | :--- |
| **Trend-Scanning Labels** | López de Prado (*Machine Learning for Asset Managers*, 2018, Ch. 5) | **Highly Practical.** Eliminates arbitrary horizontal caps. Tradeoff: Requires a forward-looking window in training, but is perfectly backtestable and easily compiled into live execution. |
| **Optimal Stopping via OU Mean-Reversion** | Bertram (2010), AFML Ch. 13 | **Excellent for Fades.** Mathematically solves the optimal exit threshold for mean-reverting price paths. Tradeoff: Prone to parameter-estimation error if the half-life of mean reversion shifts rapidly. |
| **Deep Reinforcement Learning (RL) Exits** | Spooner et al. (2018) | **Unusable for Parity.** Learns dynamic optimal exits (trailing/scaling) natively. Tradeoff: Extremely high sample complexity, prone to severe overfitting, and makes backtesting completely dependent on highly fragile simulation models. |

---

## 4. Architectural Subtleties & Structural Defects

### Sample Concurrency & Sequential Trading (AFML Ch. 4)
Because your system trades sequentially (one position at a time), the concurrency index $c_t$ equals 1, meaning you do not have overlapping concurrent positions of the same instrument. While this eliminates the need for sample-uniqueness weighting (Ch. 4's concurrent-sample adjustment), it introduces a severe **survival/selection bias**. 

If Signal A enters a trade, subsequent Signals B and C are ignored while the trade is active. In the backtest, if the exit of Signal A is poorly modeled (due to trailing/ladder mismatches), the entry eligibility of Signals B and C becomes corrupted, leading to phantom performance discrepancies. Strict, deterministic single-stage exits are required to maintain the chronological integrity of sequential execution.

### Two Controllers on One Stop Order: A Critical Defect
The existence of a server-side trailing stop *and* a per-tick stop-modifier (two independent controller loops modifying the same stop order under different logic) is **strictly an architectural defect**.

In institutional quantitative systems, this configuration represents a severe operational failure risk:
1. **Race Conditions:** Tick-by-tick modifications from the C++ DLL and asynchronous updates from the server-side controller will collide, causing Sierra Chart to reject order modifications due to sequence number mismatches.
2. **Execution Orphans:** If one controller cancels or fills a portion of the position, the other controller may hold stale state, leaving an orphaned, unmanaged stop order active in the market (exposing the fund to infinite tail risk).
3. **State Desynchronization:** It violates the Single Source of Truth (SSOT) principle.

**Adjudication:** A single, unified component—the `TripleBarrierExitManager`—must own the position state and serve as the sole controller of order submission, modification, and cancellation.

---

## 5. Decisive Recommendation

**Adopt Option A (Single-Stage, Immutable, Volatility-Scaled Bracket with a Regime-Conditioned Vertical Barrier) as the sole execution architecture for Phase 1.** 

*The one-sentence reason it beats the alternatives:*  
It enforces absolute mathematical parity with the supervised training labels, completely eliminates the catastrophic race-condition risk of dual-controller stop modifications, and lays the necessary, uncorrupted foundation for adding dynamically-scaled targets (via Trend-Scanning) in a later phase.

---
*Adjudication report saved securely at `/home/rcruz/devel/VSCode/MindfulTrader/docs/ADR/gemini_adjudication_doctrine.md`.*
