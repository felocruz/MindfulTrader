// test_kurtosis_gate_logic.cpp — native tests for dual-clock kurtosis gate logic.
// Build & run natively:
//   g++ -std=c++17 -I include tests/cpp/test_kurtosis_gate_logic.cpp -o /tmp/kgl_test && /tmp/kgl_test

#include "KurtosisGateLogic.h"
#include <cstdio>
#include <limits>

namespace {
int g_failures = 0;

void check(const char* name, bool ok) {
    if (ok) {
        std::printf("  PASS  %s\n", name);
    } else {
        ++g_failures;
        std::printf("  FAIL  %s\n", name);
    }
}
}  // namespace

int main() {
    check("halts when slow exceeds its own threshold",
          ShouldHaltOnKurtosis(2.0f, 1.8401f, 0.0f, 1.8401f));
    check("halts when fast exceeds its own threshold, slow calm",
          ShouldHaltOnKurtosis(0.5f, 1.8401f, 2.0f, 1.8401f));
    check("does not halt when both are calm",
          !ShouldHaltOnKurtosis(0.5f, 1.8401f, 0.5f, 1.8401f));

    check("enters on slow alone", ShouldEnterKurtosisCrisis(2.0f, 1.8f, 0.0f, 1.8f));
    check("enters on fast alone, slow calm", ShouldEnterKurtosisCrisis(0.5f, 1.8f, 2.0f, 1.8f));
    const float nanFast = std::numeric_limits<float>::quiet_NaN();
    check("NaN fast reading does not spuriously enter",
          !ShouldEnterKurtosisCrisis(0.5f, 1.8f, nanFast, 1.8f));

    check("exits when slow drops below exit threshold", ShouldExitKurtosisCrisis(1.0f, 1.5f));
    check("does not exit while slow stays elevated", !ShouldExitKurtosisCrisis(1.6f, 1.5f));

    check("penalty guard trips on fast alone",
          ShouldApplyFragilityPenalty(1.0f, 1.3248f, 2.0f, 1.3248f));
    check("penalty guard stays closed when both below guard",
          !ShouldApplyFragilityPenalty(1.0f, 1.3248f, 1.0f, 1.3248f));

    check("chase cap trips on fast kurtosis alone",
          ShouldCapChase(10.0f, 4.0f, 1.0f, 2.0f, 1.8530f));
    check("chase cap trips on low DOF alone",
          ShouldCapChase(3.0f, 4.0f, 1.0f, 1.0f, 1.8530f));
    check("chase cap stays open when nothing crosses",
          !ShouldCapChase(10.0f, 4.0f, 1.0f, 1.0f, 1.8530f));

    check("crash regime trips on Amihud percentile alone",
          IsCrashRegime(10.0f, 4.0f, 1.0f, 1.0f, 1.8530f, 0.95f, 0.90f));
    check("crash regime stays false when nothing crosses",
          !IsCrashRegime(10.0f, 4.0f, 1.0f, 1.0f, 1.8530f, 0.50f, 0.90f));

    const double premiumFastDriven = ComputeTailRiskPremium(
        0.0, 1.0, 2.5, 1.6414, 2.0064, 0.0, 4.0, 8.0);
    check("tail-risk premium rises above 1.0 on fast kurtosis alone", premiumFastDriven > 1.0);

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
