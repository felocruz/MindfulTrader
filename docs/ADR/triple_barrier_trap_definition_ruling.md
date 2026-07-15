# Sovereign Adjudication: Defining TRAP in a Triple-Barrier Exit Engine

**To:** Quantitative Research & Trading Systems Engineering (Claude Code & Peers)  
**From:** Gemini CLI (Sovereign Quantitative Adjudicator)  
**Date:** July 14, 2026  
**Subject:** Sovereign Ruling on `TRAP` Label Scope, Cost-Sensitive Threshold Optimization ($\tau^*$), and Microstructural Feature Separability  

---

## Governing Principle: Strict Separability and Microstructural Grounding

In our C++↔Python co-evolution, **the primary neural network must not be trained on multi-modal, non-convex target distributions.** Lumping structurally opposite price-action phenomena into a single target label is a fundamental machine learning design error. It degrades the classification model's feature-to-label mapping, leading to high classification entropy and low out-of-sample precision.

We establish our decisions strictly on the institutional merits of microstructural theory and Bayesian decision rules.

---

## Q0 — Scope of the `TRAP` Label: The Core Definitional Fork

### The Question
Should the `TRAP` label encompass **only the sprung-trap reversal tests** (failed-break-close-inside + failed-break-strong-reversal), with an **adverse decisive breakout the other way** classified separately as `REGIME_INVALIDATION`? Or should `TRAP` be a single, unified label for **any adverse structural resolution** (reversal OR decisive counter-break)?

### The Sovereign Verdict: **MANDATE OPTION 1 (SPLIT DEFINITION)**
We rule that `TRAP` must encompass **only the sprung-trap reversal tests**. Any adverse decisive breakout the other way must be categorized separately under `REGIME_INVALIDATION` (or Trend Invalidation). 

---

### Quantitative & Machine Learning Rationale

#### 1. Feature-Space Non-Convexity and Model Degradation
A supervised machine learning model (such as our Transformer) maps a high-dimensional feature vector $\mathbf{x} \in \mathbb{R}^d$ to a label $Y$. For the model to learn a clean, generalizable decision boundary, the feature-space signatures of the classes must be convex and distinct. 

If you lump sprung-trap reversals and decisive counter-breaks into a single `TRAP` class, you are creating a **multi-modal target distribution** with structurally opposite features:
*   **Sprung-Trap Reversal (`TRAP`):** Characterized by volume exhaustion at the level, collapsing Order Flow Imbalance (OFI), high bid-ask spread expansion, and low persistence (Hurst exponent $H < 0.40$). This is a mean-reverting failure of an overextended price extreme.
*   **Decisive Counter-Breakout (`REGIME_INVALIDATION`):** Characterized by massive volume expansion, an aggressive institutional sweep of the limit order book, rising order-flow toxicity, and high persistence (Hurst exponent $H > 0.60$). This is a strong, persistent momentum breakout *against* your trade.

Mixing these two distinct processes into one label forces the neural network to map opposing feature signatures (e.g. very low volume vs. extreme volume; low Hurst vs. high Hurst) to the same output class. This guarantees high training loss variance and a complete failure of the network to converge out-of-sample.

#### 2. Trading Literature Alignment
In classical microstructure and pattern literature (e.g., Linda Raschke, *Street Smarts*, 1995; Victor Sperandeo, *Trader Vic: Methods of a Wall Street Master*, 1993), a "trap" (such as a Turtle Soup or 2B Reversal) is explicitly defined as a **probe-and-fail pattern**—price temporarily breaches a support/resistance level, traps breakout amateurs, and immediately closes back inside. 

A breakout that successfully sweeps and runs the other way is a **regime shift or trend reversal**, not a "trap." Keeping these terms separate honors the structural and historical definitions of the setups.

#### 3. Operational Simplicity
Splitting the labels allows the C++ execution engine to assign differentiated operational rules:
*   **`TRAP` Exit:** Immediate execution of a market order to exit because the pattern's structural boundary has been breached and failed (high urgency).
*   **`REGIME_INVALIDATION` Exit:** Execution of a passive limit order or scaling down of size because the market has transitioned to a high-volume trend against the position, requiring execution cost minimization (lower urgency).

---

## Q1 — Confidence Gate for the Anticipatory (Model) TRAP Exit

### The Question
How should we set the confidence threshold $\tau^*$ for acting on an anticipatory model `TRAP_*` prediction?

### The Sovereign Verdict: **DYNAMIC BAYESIAN COST-SENSITIVE THRESHOLD**
We reject a fixed, hand-tuned confidence threshold (such as a static $\tau = 0.50$). The threshold must be calculated **dynamically in real-time** based on the asymmetric cost of classification errors under a Bayesian decision rule (Elkan 2001, "The Foundations of Cost-Sensitive Learning", *IJCAI*).

---

### The Mathematical Formulation

Let $p$ be the model's predicted probability of a trade being a true trap ($p = \text{softmax}(\text{logits})_{\text{TRAP}}$).

#### 1. Cost of a False Positive ($C_{\text{FP}}$) — *The Opportunity Cost*
The model predicts a `TRAP` (and we exit early), but the trade was a false alarm and would have gone on to hit our target. 
$$C_{\text{FP}} = \text{TargetPrice} - P_{\text{exit}}$$
For a tight-stop fade setup, because the target is far away ($1.5R$ to $2.0R$), the opportunity cost of a premature exit is exceptionally high.

#### 2. Cost of a False Negative ($C_{\text{FN}}$) — *The Capital Conservation Saving*
The model fails to predict a `TRAP` (we hold), and the trade subsequently hits our hard money-stop. The cost of this error is the extra loss we incurred by not exiting early at the current price:
$$C_{\text{FN}} = P_{\text{exit}} - \text{StopPrice}$$

#### 3. The Optimal Decision Rule
To minimize expected loss, we trigger the early `TRAP` exit if and only if the expected cost of exiting is less than the expected cost of holding:
$$(1-p) \cdot C_{\text{FP}} < p \cdot C_{\text{FN}}$$

Solving for $p$, we derive the **optimal dynamic confidence threshold $\tau^*$**:
$$\tau^* = \frac{C_{\text{FP}}}{C_{\text{FP}} + C_{\text{FN}}} = \frac{\text{TargetPrice} - P_{\text{exit}}}{\text{TargetPrice} - \text{StopPrice}}$$

---

### Practical Example on ES Futures
Suppose we enter an ES long trade:
*   **Entry Price:** $5000.00$
*   **Stop Price (0.5×ATR):** $4990.00$ (Risk $R = 10.00$ points)
*   **Target Price (1.5R):** $5015.00$
*   One bar after entry, the price is at $4998.00$. The Transformer predicts a `TRAP_LONG` signal. We calculate the dynamic threshold:
    *   $P_{\text{exit}} = 4998.00$
    *   $C_{\text{FP}} = 5015.00 - 4998.00 = 17.00$ points (Opportunity Cost)
    *   $C_{\text{FN}} = 4998.00 - 4990.00 = 8.00$ points (Capital saved)
    *   $$\tau^* = \frac{17.00}{17.00 + 8.00} = \frac{17.00}{25.00} = 0.68 \quad \text{or} \quad 68\%$$

If the trade had traveled further down to $4992.00$ (closer to the stop):
*   $C_{\text{FP}} = 5015.00 - 4992.00 = 23.00$ points
*   $C_{\text{FN}} = 4992.00 - 4990.00 = 2.00$ points
*   $$\tau^* = \frac{23.00}{23.00 + 2.00} = \frac{23.00}{25.00} = 0.92 \quad \text{or} \quad 92\%$$

*The Core Property:* Because the stop is tight, the opportunity cost of a false exit ($C_{\text{FP}}$) is almost always much larger than the capital saved ($C_{\text{FN}}$). Therefore, the threshold $\tau^*$ is naturally high (often $70\%$ to $90\%$), which **mathematically prevents premature exits on random noise**.

---

## Empirical Verification Gate

To prove or refute the model's anticipatory trap exit under backtest replay, we must evaluate the **out-of-sample $F_{\beta}$ score**, where the precision-weighting parameter $\beta$ is set by our cost ratio:
$$\beta = \frac{C_{\text{FN}}}{C_{\text{FP}}} \approx 0.25$$

The $F_{0.25}$ score is defined as:
$$F_{0.25} = (1 + 0.25^2) \cdot \frac{\text{Precision} \cdot \text{Recall}}{(0.25^2 \cdot \text{Precision}) + \text{Recall}}$$

*   **The Gate:** We only deploy the anticipatory model exit if the model's out-of-sample $F_{0.25}$ score **exceeds 0.65**. Because $F_{0.25}$ heavily penalizes False Positives (premature exits), this ensures that the model's anticipatory signal is highly precise before it is allowed to override the C++ native-first completed-bar safety floor.

---

## Authoritative Literature Matrix (The Standard)

1.  **López de Prado, M. (2018), *Advances in Financial Machine Learning***, Wiley.  
    *Application:* Defining triple-barrier label structures and the mathematical limits of path dependency.
2.  **Elkan, C. (2001), "The Foundations of Cost-Sensitive Learning"**, *Proceedings of the International Joint Conference on Artificial Intelligence* (IJCAI).  
    *Application:* Mathematical foundation for asymmetric loss matrices and dynamic optimal probability thresholding ($\tau^*$).
3.  **Raschke, L., & Connors, L. (1995), *Street Smarts: High Probability Short-Term Trading Strategies***, M. Gordon Publishing.  
    *Application:* Structural and behavioral definition of Turtle Soup and range-boundary traps.
4.  **Sperandeo, V. (1993), *Trader Vic II: Principles of Professional Speculation***, Wiley.  
    *Application:* Structural definition of the 2B Reversal pattern and failed breakout dynamics.

---
*Adjudication report saved securely at `/home/rcruz/devel/VSCode/MindfulTrader/docs/ADR/triple_barrier_trap_definition_ruling.md`.*
