// tools/test_context_cache_key.cpp
// Build & run: mamba run -n mts g++ -std=c++17 -Iinclude tools/test_context_cache_key.cpp \
//   -o /tmp/context_cache_key_test && /tmp/context_cache_key_test
#include "../tools/context_cache_key.h"
#include <cstdio>
#include <cstdlib>

namespace {
int g_failures = 0;
void check(const char* name, bool ok) {
    if (ok) { std::printf("  PASS  %s\n", name); }
    else { ++g_failures; std::printf("  FAIL  %s\n", name); }
}
}  // namespace

int main() {
    const FileStat current{1000, 5000};

    check("no parquet file at all -> full rebuild",
          DecideRebuildPlanFromStats(current, std::nullopt, 240, /*parquet_exists=*/false)
              == RebuildPlan::kFull);

    check("parquet exists but no prior cache key -> full rebuild",
          DecideRebuildPlanFromStats(current, std::nullopt, 240, /*parquet_exists=*/true)
              == RebuildPlan::kFull);

    {
        ContextCacheKey prior{1000, 5000, 240, kContextParquetCacheFormatVersion, 1000, 7, true};
        check("identical size/mtime/schema -> fresh (no rebuild needed)",
              DecideRebuildPlanFromStats(current, prior, 240, true) == RebuildPlan::kFresh);
    }
    {
        ContextCacheKey prior{800, 4000, 240, kContextParquetCacheFormatVersion, 800, 5, true};
        check("file strictly grew, same schema -> incremental",
              DecideRebuildPlanFromStats(current, prior, 240, true) == RebuildPlan::kIncremental);
    }
    {
        ContextCacheKey prior{1200, 6000, 240, kContextParquetCacheFormatVersion, 1200, 9, true};
        check("file shrank -> full rebuild (content diverged from what cache reflects)",
              DecideRebuildPlanFromStats(current, prior, 240, true) == RebuildPlan::kFull);
    }
    {
        // Same size as current but different mtime -> same-size-different-content case,
        // matches materialize_context_parquet.py's own documented "schema changed, file
        // shrank, or same size with different content" full-rebuild branch.
        ContextCacheKey prior{1000, 4999, 240, kContextParquetCacheFormatVersion, 1000, 7, true};
        check("same size, different mtime -> full rebuild",
              DecideRebuildPlanFromStats(current, prior, 240, true) == RebuildPlan::kFull);
    }
    {
        ContextCacheKey prior{800, 4000, 230, kContextParquetCacheFormatVersion, 800, 5, true};
        check("file grew but WIRE schema_version differs -> full rebuild, never incremental "
              "across a wire-format change",
              DecideRebuildPlanFromStats(current, prior, 240, true) == RebuildPlan::kFull);
    }
    {
        ContextCacheKey prior{800, 4000, 240, kContextParquetCacheFormatVersion + 1, 800, 5, true};
        check("file grew but PARQUET OUTPUT format version differs -> full rebuild "
              "(distinct from wire schema_version -- this is the tool's own output shape)",
              DecideRebuildPlanFromStats(current, prior, 240, true) == RebuildPlan::kFull);
    }

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
