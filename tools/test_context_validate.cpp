// tools/test_context_validate.cpp
// Build & run: mamba run -n mts g++ -std=c++17 -Iinclude tools/test_context_validate.cpp \
//   -o /tmp/context_validate_test && /tmp/context_validate_test
#include "../tools/context_validate_stats.h"
#include <cmath>
#include <cstdio>
#include <limits>

namespace {
int g_failures = 0;
void check(const char* name, bool ok) {
    if (ok) { std::printf("  PASS  %s\n", name); }
    else { ++g_failures; std::printf("  FAIL  %s\n", name); }
}
bool close(double a, double b, double eps = 1e-9) { return std::fabs(a - b) < eps; }
}  // namespace

int main() {
    // dim0 = [0, 0, 0, 1, 2]; dim1 = [1, 1, 1, 1, NaN] -- row 4 has a NaN in dim1,
    // so row 4 is excluded from BOTH dims' filtered stats (row-level mask), but
    // dim1's raw nan_inf_count still counts it (unfiltered).
    std::vector<std::vector<float>> columns = {
        {0.0f, 0.0f, 0.0f, 1.0f, 2.0f},
        {1.0f, 1.0f, 1.0f, 1.0f, std::numeric_limits<float>::quiet_NaN()},
    };

    auto mask = ComputeRowFiniteMask(columns, 5);
    check("rows 0-3 are finite", mask[0] && mask[1] && mask[2] && mask[3]);
    check("row 4 is excluded (dim1 NaN)", !mask[4]);

    auto stats = ComputeDimStats(columns);
    check("2 dims computed", stats.size() == 2);
    check("dim0 nan_inf_count is 0 (never non-finite)", stats[0].nan_inf_count == 0);
    check("dim1 nan_inf_count is 1 (raw, unfiltered)", stats[1].nan_inf_count == 1);
    check("dim0 finite_row_count is 4 (row 4 dropped by row-level mask)",
          stats[0].finite_row_count == 4);
    check("dim0 mean over [0,0,0,1] is 0.25", close(stats[0].mean, 0.25));
    check("dim0 zero_ratio is 0.75", close(stats[0].zero_ratio, 0.75));
    check("dim0 min/max are 0/1", close(stats[0].min, 0.0) && close(stats[0].max, 1.0));
    check("dim1 mean over [1,1,1,1] is 1.0", close(stats[1].mean, 1.0));
    check("dim1 one_ratio is 1.0", close(stats[1].one_ratio, 1.0));
    check("dim1 constant_ratio is 1.0", close(stats[1].constant_ratio, 1.0));
    check("dim1 std_dev is 0.0 (constant column)", close(stats[1].std_dev, 0.0));

    {
        // dim0=[1,2,3,4], dim1=[2,4,6,8] (perfectly correlated, corr=+1.0),
        // dim2=[4,3,2,1] (perfectly anti-correlated with dim0, corr=-1.0).
        std::vector<std::vector<float>> corr_columns = {
            {1.0f, 2.0f, 3.0f, 4.0f},
            {2.0f, 4.0f, 6.0f, 8.0f},
            {4.0f, 3.0f, 2.0f, 1.0f},
        };
        auto corr = ComputeCorrelationMatrix(corr_columns);
        check("corr matrix is 3x3", corr.size() == 3 && corr[0].size() == 3);
        check("dim0/dim1 perfectly correlated", close(corr[0][1], 1.0, 1e-6));
        check("dim0/dim2 perfectly anti-correlated", close(corr[0][2], -1.0, 1e-6));
        check("matrix is symmetric", close(corr[1][0], corr[0][1]));
        check("diagonal is 1.0", close(corr[0][0], 1.0, 1e-6) && close(corr[1][1], 1.0, 1e-6));

        auto top = TopCorrelationPairs(corr, 2);
        check("top-2 pairs returned", top.size() == 2);
        check("top pair has abs_corr 1.0", close(top[0].abs_corr, 1.0, 1e-6));

        auto above = PairsAboveThreshold(corr, 0.8);
        // dim1 = 2*dim0 and dim2 = reverse(dim0), so ALL THREE pairs are exactly
        // +-1.0 (verified independently against real np.corrcoef, not assumed):
        // (dim0,dim1)=+1, (dim0,dim2)=-1, (dim1,dim2)=-1.
        check("all 3 pairs exceed 0.8 abs corr threshold", above.size() == 3);
    }

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
