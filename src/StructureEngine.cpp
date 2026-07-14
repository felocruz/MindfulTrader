#include "StructureEngine.h"
#include <numeric>
#include <cmath>
#include <algorithm>
#include <map>

namespace MindfulTrader {

void StructureEngine::Reset() {
    m_prices.clear();
    m_highs.clear();
    m_lows.clear();
    m_logRanges.clear();
}

void StructureEngine::Update(float high, float low, float close, bool isNewBar) {
    if (isNewBar) {
        m_prices.push_back(close);
        m_highs.push_back(high);
        m_lows.push_back(low);
        
        float range = std::max(high - low, 1e-5f); // Prevent log(0)
        m_logRanges.push_back(std::log(range));

        if (m_prices.size() > WINDOW_SIZE) m_prices.pop_front();
        if (m_highs.size() > WINDOW_SIZE) m_highs.pop_front();
        if (m_lows.size() > WINDOW_SIZE) m_lows.pop_front();
        if (m_logRanges.size() > EXPANSION_WINDOW) m_logRanges.pop_front();
    } else if (!m_prices.empty()) {
        // Overwrite latest values
        m_prices.back() = close;
        m_highs.back() = high;
        m_lows.back() = low;
        
        float range = std::max(high - low, 1e-5f); // Prevent log(0)
        m_logRanges.back() = std::log(range);
    } else {
        // Strict mode: reject update when stream has no active bar context.
        return;
    }
}

float StructureEngine::GetLogExpansionRatio() const {
    if (m_logRanges.empty()) return 1.0f;
    
    // Current bar log-range
    float currentLogRange = m_logRanges.back();
    
    // Average historical log-range (100-bar baseline)
    float sum = std::accumulate(m_logRanges.begin(), m_logRanges.end(), 0.0f);
    float avg = sum / m_logRanges.size();
    
    // Ratio: If current > avg, expanding. If < avg, consolidating.
    // Exp(current - avg) gives strictly positive ratio.
    // e.g. ln(10) - ln(5) = ln(2) -> exp(ln(2)) = 2.0.
    return std::exp(currentLogRange - avg);
}

float StructureEngine::GetRecurrenceRate() const {
    if (m_prices.size() < WINDOW_SIZE) return 0.5f;

    // Use a simple histogram to find the Point of Control (Mode)
    // Bin size = approximate ATR or just dynamic range / 10
    float minP = *std::min_element(m_prices.begin(), m_prices.end());
    float maxP = *std::max_element(m_prices.begin(), m_prices.end());
    float range = maxP - minP;
    if (range < 1e-5f) return 1.0f; // Flat line = 100% recurrence

    int bins = 10;
    float binSize = range / bins;
    std::vector<int> histogram(bins, 0);
    
    for (float p : m_prices) {
        int bin = std::min(static_cast<int>((p - minP) / binSize), bins - 1);
        histogram[bin]++;
    }

    // Find strictly dominant bin (Mode)
    int maxCount = *std::max_element(histogram.begin(), histogram.end());
    
    // Recurrence Rate: % of time in the mode bin
    return static_cast<float>(maxCount) / m_prices.size();
}

float StructureEngine::GetFractalDimension() const {
    if (m_prices.size() < WINDOW_SIZE) return 1.5f; // Neutral Brownian

    // Simple robust estimator: Path Length / Log-Range
    // D = log(L) / log(d) is Hausdorff, but for time series we use Sevcik's or similar.
    // Let's use a simpler "Roughness" proxy:
    // Path Length L = sum(|close_i - close_{i-1}|)
    // Displacement D = max_high - min_low
    // Normalized L is scale-invariant.
    
    float pathLength = 0.0f;
    for (size_t i = 1; i < m_prices.size(); ++i) {
        pathLength += std::abs(m_prices[i] - m_prices[i-1]);
    }
    
    float minP = *std::min_element(m_lows.begin(), m_lows.end());
    float maxP = *std::max_element(m_highs.begin(), m_highs.end());
    float displacement = std::max(maxP - minP, 1e-5f);

    // If path == displacement (straight line), ratio = 1.
    // If path >> displacement (noise), ratio is high.
    // Map to [1, 2] roughly via log-log relationship for Hurst proxy.
    // But let's return the raw efficient Dimensionless Roughness Ratio:
    return pathLength / displacement; 
}

float StructureEngine::GetMeanReversionZ() const {
    if (m_prices.size() < 10) return 0.0f;

    // Calculate OLS: Price = a + b * Time
    auto [slope, intercept] = CalculateOLS();
    
    // Current theoretical price (at last index)
    float t = static_cast<float>(m_prices.size() - 1);
    float predicted = intercept + slope * t;
    float current = m_prices.back();
    
    // Residual
    float residual = current - predicted;
    
    // Standard deviation of historical residuals
    float stdRes = CalculateResidualStdDev(slope, intercept);
    if (stdRes < 1e-5f) return 0.0f;
    
    // Z-Score
    return residual / stdRes;
}

std::pair<float, float> StructureEngine::CalculateOLS() const {
    float n = static_cast<float>(m_prices.size());
    float sumX = 0, sumY = 0, sumXY = 0, sumXX = 0;
    
    for (size_t i = 0; i < m_prices.size(); ++i) {
        float x = static_cast<float>(i);
        float y = m_prices[i];
        sumX += x;
        sumY += y;
        sumXY += x * y;
        sumXX += x * x;
    }
    
    float denominator = n * sumXX - sumX * sumX;
    if (std::abs(denominator) < 1e-5f) return {0.0f, m_prices.back()};
    
    float slope = (n * sumXY - sumX * sumY) / denominator;
    float intercept = (sumY - slope * sumX) / n;
    
    return {slope, intercept};
}

float StructureEngine::CalculateResidualStdDev(float slope, float intercept) const {
    float sumSqRes = 0.0f;
    for (size_t i = 0; i < m_prices.size(); ++i) {
        float x = static_cast<float>(i);
        float y = m_prices[i];
        float predicted = intercept + slope * x;
        float res = y - predicted;
        sumSqRes += res * res;
    }
    return std::sqrt(sumSqRes / m_prices.size());
}

} // namespace MindfulTrader
