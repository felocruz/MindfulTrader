#pragma once

#include <algorithm>
#include <cmath>

inline bool ShouldHaltOnKurtosis(float slowKurtosis, float slowThreshold,
                                  float fastKurtosis, float fastThreshold) {
    return slowKurtosis > slowThreshold ||
           (std::isfinite(fastKurtosis) && fastKurtosis > fastThreshold);
}

inline bool ShouldEnterKurtosisCrisis(float slowKurtosis, float slowEnterThreshold,
                                       float fastKurtosis, float fastEnterThreshold) {
    return slowKurtosis > slowEnterThreshold ||
           (std::isfinite(fastKurtosis) && fastKurtosis > fastEnterThreshold);
}

inline bool ShouldExitKurtosisCrisis(float slowKurtosis, float slowExitThreshold) {
    return slowKurtosis < slowExitThreshold;
}

inline bool ShouldApplyFragilityPenalty(float slowKurtosis, float slowGuard,
                                         float fastKurtosis, float fastGuard) {
    return slowKurtosis > slowGuard ||
           (std::isfinite(fastKurtosis) && fastKurtosis > fastGuard);
}

inline bool ShouldCapChase(float dof, float dofThreshold,
                            float slowKurtosis, float fastKurtosis, float kurtosisThreshold) {
    return dof <= dofThreshold ||
           slowKurtosis > kurtosisThreshold ||
           (std::isfinite(fastKurtosis) && fastKurtosis > kurtosisThreshold);
}

inline bool IsCrashRegime(float dof, float dofThreshold,
                           float slowKurtosis, float fastKurtosis, float kurtosisThreshold,
                           float amihudPercentile, float amihudThreshold) {
    return dof <= dofThreshold ||
           slowKurtosis > kurtosisThreshold ||
           (std::isfinite(fastKurtosis) && fastKurtosis > kurtosisThreshold) ||
           amihudPercentile > amihudThreshold;
}

inline double ComputeTailRiskPremium(double hillPenalty,
                                      double slowKurtosis, double fastKurtosis,
                                      double kurtosisPenaltyLow, double kurtosisPenaltyHigh,
                                      double mahalanobis,
                                      double mahalPenaltyLow, double mahalPenaltyHigh) {
    const double kurtosisPenalty = std::clamp(
        (slowKurtosis - kurtosisPenaltyLow) /
            (kurtosisPenaltyHigh - kurtosisPenaltyLow), 0.0, 1.0);
    const double fastKurtosisPenalty = std::clamp(
        (fastKurtosis - kurtosisPenaltyLow) /
            (kurtosisPenaltyHigh - kurtosisPenaltyLow), 0.0, 1.0);
    const double mahalPenalty = std::clamp(
        (mahalanobis - mahalPenaltyLow) /
            (mahalPenaltyHigh - mahalPenaltyLow), 0.0, 1.0);
    return 1.0 + std::max({hillPenalty, kurtosisPenalty, fastKurtosisPenalty, mahalPenalty});
}
