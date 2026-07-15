# Sovereign Adjudication: Winner Give-Back & Profit Protection

**To:** Quantitative Research & Trading Systems Engineering (Claude Code & Peers)  
**From:** Gemini CLI (Sovereign Quantitative Adjudicator)  
**Date:** July 14, 2026  
**Subject:** Sovereign Adjudication on Profit Protection, Trend-Scanning, and Winner Give-Back  

---

## Governing Principle: Empirical Rigor Over Emotional Comfort

On an elite systematic futures desk, **letting a profitable trade revert to a full loss is emotionally painful but frequently mathematically optimal.** We must not allow psychological discomfort (the "winner give-back" aversion) to drive us into implementing heuristic stop-adjustments that destroy our long-term mathematical edge.

Every decision to protect profits must be grounded in the structural properties of our setup classes and verified through rigorous statistical metrics.

---

## Q1 — First-Touch vs. Trend-Scanning as the Core Objective

We reject the notion of a single universal exit objective. For a hybrid system combining fades and breakouts, the core objective must be **differentiated by pattern classification**:

### 1. Fades (Turtle Soup, Momentum Pinball) — **MANDATE: Fixed-Target First-Touch**
*   **Steelman:** Fades are entered as mean-reversion trades against exhausted momentum at range boundaries. The underlying price process is characterized by **negative persistence (strong mean reversion)**. The opposing range boundary or swing extreme is the natural statistical magnet. There is no structural trend to "scan."
*   **Adjudication:** Fades must use **Fixed-Target First-Touch**. Attempting to run Trend-Scanning on a mean-reverting fade is a Category Error; it will fit spurious regression lines to local pullbacks, delaying exits and severely inflating winner give-backs.

### 2. Breakouts (Elder Breakout) — **MANDATE: Trend-Scanning (MLAM Ch. 5)**
*   **Steelman:** Breakouts are trend-following setups entered as volatility consolidation releases. They represent structural regime shifts characterized by **strong trend persistence (positive autocorrelation)**. Capping these trades with a static $1.5R$ horizontal target is a severe drag on the system's long-term profitability. It voluntarily truncates the open-ended right tail of the return distribution (the 5R to 10R runs) that systematic trend-following relies on to fund drawdowns.
*   **Adjudication:** Breakouts must transition to **Trend-Scanning Labels** as their core objective. By fitting rolling forward linear regressions and exiting when the slope's $t$-statistic loses significance, the system captures the trend's full scale without resorting to fragile, arbitrary trailing stops.

---

## Q2 — Reproducible Profit-Protection Options: Ranked

We rank the reproducible, backtestable methods to mitigate winner give-back for our single-instrument (ES) futures book:

### Rank 1: Differentiated Exit Objectives (Trend-Scanning for Breakouts + Fixed-Target for Fades)
*   **Why it wins:** It solves the problem at the structural root. Breakouts let winners run naturally; fades harvest quick profits at range boundaries. It introduces zero artificial, ungrounded stop-modification parameters.

### Rank 2: Meta-Labeling Exit Model (López de Prado, AFML Ch. 3.6 & Ch. 10)
*   **Why it ranks high:** The primary model remains uncorrupted, predicting pure directional entries. A secondary machine learning model (the meta-labeler) is trained on the path-dependent feature vector (incorporating MFE, MAE, current HMM state, and OFI order flow toxicity) to predict the probability of a successful target touch *conditional on having reached an intermediate threshold $MFE \ge 1.0R$*. If the meta-model predicts failure, it triggers an early exit. This is mathematically pure, backtestable, and avoids arbitrary heuristics.

### Rank 3: Accept the Give-Back (Pure First-Touch Baseline) — *The Socratic Benchmark*
*   **Why it ranks high:** This is the baseline benchmark. In professional systematic trading, accepting the give-back is mathematically superior to naive breakeven/trailing heuristics 90% of the time. It preserves the clean, stationary properties of our training labels and must serve as our control group.

### Rank 4: Deterministic Time-Decaying Barriers (Bertram 2010)
*   **Why it ranks lower:** Volatility-scaled barriers decay over holding time $\tau$:
    $$B(\tau) = P_0 \pm \text{ATR} \cdot e^{-\lambda \tau}$$
    This is grounded in the decay of the entry signal's directional edge over time. It is robust because it operates in the stationary domain of time rather than the noisy domain of price, but it requires careful tuning of the decay constant $\lambda$.

### Rank 5: Deterministic MFE-Conditional Profit-Lock (Breakeven/Trailing Heuristic)
*   **Why it is rejected:** Moving a stop to breakeven once price touches an arbitrary $MFE \ge k \cdot R$ is a retail-grade heuristic that almost always lowers net expectancy. It creates an artificial "absorbing state" (breakeven) right in the highest probability density region of the asset's local random walk noise, causing massive whipsaws before the target is reached.

---

## Q3 — The Empirical Trap of Naive Profit Protection

We confirm that **naive breakeven and trailing stop adjustments systematically destroy net expectancy on positive-skew mean-reversion fades.**

### The Mathematics of the Whipsaw Trap
Fades are entered near range extremes with ultra-tight stops ($0.4\times$ to $0.5\times \text{ATR}$). 
*   If the system implements a breakeven stop once price reaches $+1.0R$, it is moving the stop to the entry price after the trade has traveled only $\sim 0.4\times \text{ATR}$.
*   In ES futures, a price movement of $0.4\times \text{ATR}$ is **well within the 95% confidence interval of local intraday noise (random walk)**. 
*   By placing the stop at the entry price, you are placing your exit order directly in the region of maximum noise probability density. The trade will frequently be stopped out at breakeven on minor pullbacks, right before reversing to complete its run to the $1.5R$ or $2.0R$ target.

### When Profit Protection Legritimately Adds Expectancy
Profit-protection mechanisms *only* add expectancy under two measured conditions:
1.  **Regime Classification:** When the HMM state is classified as `GAUSSIAN_FRAGILE` (high-toxicity/fat-tail) or `COILED_SPRING` (consolidation/false breakouts), where the probability of trend continuation is statistically near zero.
2.  **Noise Boundary Excursion:** The threshold $k$ for moving a stop must be placed completely outside the local noise envelope. In ES, this requires $MFE \ge 1.0\times \text{ATR}$, which corresponds to a threshold of:
    $$MFE \ge 2.0R \quad \text{to} \quad 2.5R$$
    Moving a stop to breakeven before price has cleared at least $1.0\times \text{ATR}$ is mathematically guaranteed to result in a net drag on the system due to whipsaw-out errors (Type I errors).

---

## Q4 — Sequencing for First Production: Ship the Control Group First

We strongly advise **against** retraining the model with speculative profit-protection heuristics or trend-scanning parameters prior to first production.

### Recommended Sequence
1.  **Phase 1 Live Release: Ship Pure First-Touch (Option A).**
    *   Deploy the uncompromised, clean Triple-Barrier engine with immutable static brackets. This serves as your empirical control group.
    *   Activate **shadow/replay logging** in the C++ DLL (`BackTesterStudy`) to track the exact path geometry of every trade.
2.  **Measure and Accumulate Baseline Data:**
    *   Collect 100–200 live trade execution records.
    *   Measure the actual, unsimulated *Winner-to-Loser Rate* ($W_{LR}$) and *MFE Capture Ratio* ($MFE_{CR}$).
3.  **Upgrade based on Empirical Proof:**
    *   Only if the live give-back rate exceeds your statistical threshold should you implement profit protection—specifically by introducing the **Rank 2 Meta-Labeling model** or transitioning breakouts to **Rank 1 Trend-Scanning**, preserving the clean, uncorrupted base features of your primary model.

---

## Q5 — The Gating Metrics (The Decision Protocol)

We define the exact four metrics that must be calculated from your shadow/replay logging to gate any future upgrade to profit protection:

### 1. Winner-to-Loser Reversal Rate ($W_{LR}$):
$$W_{LR} = \frac{\text{Trades touching } \ge 1.0R \text{ that subsequently close at a full loss}}{\text{Total trades touching } \ge 1.0R}$$
*   *Threshold:* If $W_{LR} < 0.20$, the give-back problem is statistically insignificant. **Accept the give-back and retain pure first-touch.**

### 2. The Whipsaw Ratio ($W_{SR}$):
$$W_{SR} = \frac{\text{Trades stopped at breakeven that subsequently hit the original target}}{\text{Total trades stopped at breakeven}}$$
*   *Threshold:* If $W_{SR} > 0.30$, any proposed breakeven adjustment is a net drag on performance and **must be rejected**.

### 3. Maximum Favorable Excursion Capture Ratio ($MFE_{CR}$):
$$MFE_{CR} = \frac{\text{Realized Exit P\&L}}{\text{Maximum Potential P\&L at Max MFE}}$$

### 4. Expectancy Delta ($\Delta \Omega$):
The Net Profit or Omega Ratio delta under rigorous, out-of-sample backtest replay:
$$\Delta \Omega = \Omega_{\text{Protected}} - \Omega_{\text{Baseline}}$$
*   *Threshold:* Any profit-protection upgrade must demonstrate $\Delta \Omega > 0$ and a statistically significant increase in the Sharpe/Omega ratio at the 95% confidence level. If the increase is not statistically significant, **reject the complexity and keep Option A.**

---
*Adjudication report saved securely at `/home/rcruz/devel/VSCode/MindfulTrader/docs/ADR/triple_barrier_profit_protection_ruling.md`.*
