#pragma once

#include <vector>
#include <deque>
#include <cmath>
#include <numeric>
#include <algorithm>
#include <Eigen/Dense>

namespace MindfulTrader {

class StructureEngine {
public:
    static constexpr size_t WINDOW_SIZE = 30; // 30-bar local structure window
    static constexpr size_t EXPANSION_WINDOW = 100; // 100-bar baseline for expansion

    StructureEngine() = default;

    /// Reset internal state
    void Reset();

    /// Update with new bar data
    /// @param isNewBar: true if this is a new bar (push back), false if updating the latest bar (overwrite)
    void Update(float high, float low, float close, bool isNewBar = true);

    /// 1. Log-Expansion Ratio (Dimensionless Range Expansion)
    /// ln(current_range) / avg(ln(historical_ranges))
    float GetLogExpansionRatio() const;

    /// 2. Recurrence Rate (Topological Stability)
    /// % of time price spends in the "Value Area" (Mode/POC of window)
    float GetRecurrenceRate() const;

    /// 3. Fractal Dimension (Roughness)
    /// D = 2 - H_local (or Box Counting proxy: PathLength / Displacement)
    /// Returns [1.0, 2.0]
    float GetFractalDimension() const;

    /// 4. Mean Reversion Potential (Standardized Residual)
    /// Z-Score of current Price vs Rolling OLS trend
    float GetMeanReversionZ() const;

    bool IsReady() const { return m_prices.size() >= WINDOW_SIZE; }

private:
    std::deque<float> m_prices;       // Close prices
    std::deque<float> m_highs;        // High prices
    std::deque<float> m_lows;         // Low prices
    std::deque<float> m_logRanges;    // ln(High - Low) history

    // Helper: Simple Linear Regression (Y = a + bX)
    // Returns {slope, intercept}
    std::pair<float, float> CalculateOLS() const;
    
    // Helper: Standard Deviation of residuals
    float CalculateResidualStdDev(float slope, float intercept) const;
};

} // namespace MindfulTrader
