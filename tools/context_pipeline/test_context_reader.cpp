// tools/context_pipeline/test_context_reader.cpp
// Build & run: mamba run -n mts g++ -std=c++17 -Iinclude tools/context_pipeline/test_context_reader.cpp \
//   -o /tmp/context_reader_test && /tmp/context_reader_test
#include "context_reader.h"
#include "generated/mts_schema_generated.h"
#include "generated/mts_schema_contract_generated.h"
#include "flatbuffers/flatbuffers.h"
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace {
int g_failures = 0;
void check(const char* name, bool ok) {
    if (ok) { std::printf("  PASS  %s\n", name); }
    else { ++g_failures; std::printf("  FAIL  %s\n", name); }
}

// Writes a minimal valid .context-format file: "LBRN" magic + size-prefixed
// FileMetadata, matching LBRFileManager.cpp::Open()/WriteMagicHeader()/
// WriteFileMetadata() exactly.
void WriteTestFile(const std::string& path, std::uint16_t schema_version) {
    flatbuffers::FlatBufferBuilder fbb(256);
    auto symbol = fbb.CreateString("TEST");
    auto timeframe = fbb.CreateString("CONTEXT_v2.5_MO_SS");
    auto meta = MTS::Training::CreateFileMetadata(fbb, symbol, timeframe, schema_version, 0);
    fbb.Finish(meta);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    const char magic[4] = {'L', 'B', 'R', 'N'};
    out.write(magic, 4);
    std::uint32_t size = static_cast<std::uint32_t>(fbb.GetSize());
    out.write(reinterpret_cast<const char*>(&size), sizeof(size));
    out.write(reinterpret_cast<const char*>(fbb.GetBufferPointer()), size);
}

// Builds a minimal synthetic .context-format record stream (magic + FileMetadata
// + N MO/SS pairs, one deliberately-mismatched SS) and writes it to disk --
// exercises ProcessOneRecord/ReadAllRecords exactly the way a real collected
// file would, not raw byte forging.
void WriteSyntheticContextFile(const std::string& path, int n_pairs) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    const char magic[4] = {'L', 'B', 'R', 'N'};
    out.write(magic, 4);

    flatbuffers::FlatBufferBuilder meta_fbb(256);
    auto symbol = meta_fbb.CreateString("TEST");
    auto timeframe = meta_fbb.CreateString("CONTEXT_v2.5_MO_SS");
    auto meta = MTS::Training::CreateFileMetadata(
        meta_fbb, symbol, timeframe, MTS::Schema::Contract::kSchemaVersion, 0);
    meta_fbb.Finish(meta);
    std::uint32_t meta_size = static_cast<std::uint32_t>(meta_fbb.GetSize());
    out.write(reinterpret_cast<const char*>(&meta_size), sizeof(meta_size));
    out.write(reinterpret_cast<const char*>(meta_fbb.GetBufferPointer()), meta_size);

    for (int i = 0; i < n_pairs; ++i) {
        flatbuffers::FlatBufferBuilder mo_fbb(256);
        MTS::Schema::ObservationData obs(
            static_cast<float>(i), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        MTS::Schema::AsymmetryContext ctx(0, 0, 0, 0, 0, 0, 0, 0);
        auto mo = MTS::Schema::CreateMarketObservation(
            mo_fbb, 1000 + i, static_cast<std::uint64_t>(i), &obs, &ctx, 0, 0);
        mo_fbb.Finish(mo);
        std::uint32_t mo_size = static_cast<std::uint32_t>(mo_fbb.GetSize());
        out.write(reinterpret_cast<const char*>(&mo_size), sizeof(mo_size));
        out.write(reinterpret_cast<const char*>(mo_fbb.GetBufferPointer()), mo_size);

        flatbuffers::FlatBufferBuilder ss_fbb(128);
        // Every pair aligns except i == 1, whose SS carries a mismatched sequence_id
        // (exercises the sequence_mismatches counter, matching the Python reader's
        // own tested behavior for this exact case).
        std::uint64_t ss_seq = (i == 1) ? 9999 : static_cast<std::uint64_t>(i);
        auto ss = MTS::Schema::CreateSystemState(ss_fbb, 1000 + i, ss_seq, 5.0f + i);
        ss_fbb.Finish(ss);
        std::uint32_t ss_size = static_cast<std::uint32_t>(ss_fbb.GetSize());
        out.write(reinterpret_cast<const char*>(&ss_size), sizeof(ss_size));
        out.write(reinterpret_cast<const char*>(ss_fbb.GetBufferPointer()), ss_size);
    }
}

struct CollectedPairs {
    std::vector<float> log_scale_ratios;
    std::vector<float> bars_since_last_update;
};

void CollectPair(const PairedRecord& rec, void* user_data) {
    auto* out = static_cast<CollectedPairs*>(user_data);
    out->log_scale_ratios.push_back(rec.observation.observation->log_scale_ratio());
    out->bars_since_last_update.push_back(rec.bars_since_last_update);
}
}  // namespace

int main() {
    {
        WriteTestFile("/tmp/ctx_reader_test_good.context", MTS::Schema::Contract::kSchemaVersion);
        auto handle = OpenContextFile("/tmp/ctx_reader_test_good.context");
        check("matching schema_version opens successfully", handle.base != nullptr);
        check("record_stream_offset is past magic+FileMetadata", handle.record_stream_offset > 8);
        CloseContextFile(handle);
    }
    {
        WriteTestFile("/tmp/ctx_reader_test_stale.context", 230);
        bool threw = false;
        try {
            auto handle = OpenContextFile("/tmp/ctx_reader_test_stale.context");
            (void)handle;
        } catch (const std::runtime_error&) {
            threw = true;
        }
        check("stale schema_version (230) is hard-refused", threw);
    }
    {
        std::ofstream bad("/tmp/ctx_reader_test_badmagic.context", std::ios::binary | std::ios::trunc);
        bad.write("XXXX", 4);
        bad.close();
        bool threw = false;
        try {
            auto handle = OpenContextFile("/tmp/ctx_reader_test_badmagic.context");
            (void)handle;
        } catch (const std::runtime_error&) {
            threw = true;
        }
        check("bad magic header is rejected", threw);
    }
    {
        flatbuffers::FlatBufferBuilder fbb(512);
        MTS::Schema::ObservationData obs(
            1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f, 10.0f,
            11.0f, 12.0f, 13.0f, 14.0f, 15.0f, 16.0f, 17.0f, 18.0f, 19.0f);
        MTS::Schema::AsymmetryContext ctx(0, 0, 0, 0, 0, 0, 0, 0);
        auto rgc_offset = MTS::Schema::CreateRiskGateContext(
            fbb, /*shannon_flow_entropy=*/0.1f, /*shannon_efficiency=*/0.2f,
            /*taleb_kurtosis=*/0.3f, /*taleb_skewness=*/0.4f,
            /*elder_chandelier_atr=*/0.5f, /*pareto_tail_alpha=*/4.0f,
            /*amihud_illiquidity=*/0.6f, /*spread_stress=*/0.7f,
            /*hurst_exponent=*/0.8f, /*fractal_dim=*/1.5f,
            /*mean_rev_z=*/0.9f, /*raschke_burst=*/1.0f,
            /*fisher_info=*/1.1f, /*regime_duration=*/42,
            /*is_valid=*/true, /*snapshot_timestamp_us=*/12345, /*amihud_percentile=*/0.5f);
        auto mo = MTS::Schema::CreateMarketObservation(
            fbb, /*timestamp_us=*/999, /*sequence_id=*/7, &obs, &ctx, /*daily_bias_enum=*/0, rgc_offset);
        fbb.Finish(mo);

        auto parsed = ReadMarketObservation(fbb.GetBufferPointer());
        check("timestamp_us round-trips", parsed.timestamp_us == 999);
        check("sequence_id round-trips", parsed.sequence_id == 7);
        check("observation pointer is non-null", parsed.observation != nullptr);
        check("observation.log_scale_ratio() round-trips", parsed.observation->log_scale_ratio() == 1.0f);
        check("observation.fast_mean_rev_z() round-trips", parsed.observation->fast_mean_rev_z() == 19.0f);
        check("risk_gate_context is present", parsed.risk_gate_context != nullptr);
        check("risk_gate_context.hurst_exponent() (raw) round-trips",
              parsed.risk_gate_context->hurst_exponent() == 0.8f);
        check("risk_gate_context.regime_duration() round-trips",
              parsed.risk_gate_context->regime_duration() == 42);
    }
    {
        // No RiskGateContext supplied (live path) -- must not crash, pointer is null.
        flatbuffers::FlatBufferBuilder fbb(256);
        MTS::Schema::ObservationData obs(
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        MTS::Schema::AsymmetryContext ctx(0, 0, 0, 0, 0, 0, 0, 0);
        auto mo = MTS::Schema::CreateMarketObservation(fbb, 1, 1, &obs, &ctx, 0, 0);
        fbb.Finish(mo);
        auto parsed = ReadMarketObservation(fbb.GetBufferPointer());
        check("missing risk_gate_context yields a null pointer, not a crash",
              parsed.risk_gate_context == nullptr);
    }
    {
        WriteSyntheticContextFile("/tmp/ctx_reader_test_stream.context", 4);
        auto handle = OpenContextFile("/tmp/ctx_reader_test_stream.context");
        CollectedPairs collected;
        ReadCounters counters = ReadAllRecords(handle, CollectPair, &collected);
        check("4 pairs attempted", counters.pair_attempts == 4);
        check("3 of 4 pairs aligned (pair 1 deliberately mismatched)", counters.aligned_pairs == 3);
        check("1 sequence mismatch counted", counters.sequence_mismatches == 1);
        check("3 aligned pairs collected in original stream order",
              collected.log_scale_ratios.size() == 3 &&
              collected.log_scale_ratios[0] == 0.0f &&
              collected.log_scale_ratios[1] == 2.0f &&
              collected.log_scale_ratios[2] == 3.0f);
        CloseContextFile(handle);
    }
    {
        WriteSyntheticContextFile("/tmp/ctx_reader_test_bounded.context", 6);
        auto handle = OpenContextFile("/tmp/ctx_reader_test_bounded.context");
        CollectedPairs head_collected;
        ReadCounters head_counters = ReadBoundedHead(handle, 2, CollectPair, &head_collected);
        check("bounded head stops at exactly 2 aligned pairs",
              head_collected.log_scale_ratios.size() == 2);
        check("bounded head keeps the FIRST 2 aligned pairs in order",
              head_collected.log_scale_ratios[0] == 0.0f &&
              head_collected.log_scale_ratios[1] == 2.0f);  // pair index 1 mismatched, skipped
        (void)head_counters;
        CloseContextFile(handle);
    }
    {
        WriteSyntheticContextFile("/tmp/ctx_reader_test_bounded2.context", 6);
        auto handle = OpenContextFile("/tmp/ctx_reader_test_bounded2.context");
        CollectedPairs tail_collected;
        ReadCounters tail_counters = ReadBoundedTail(handle, 2, CollectPair, &tail_collected);
        check("bounded tail keeps the LAST 2 aligned pairs",
              tail_collected.log_scale_ratios.size() == 2 &&
              tail_collected.log_scale_ratios[0] == 4.0f &&
              tail_collected.log_scale_ratios[1] == 5.0f);
        (void)tail_counters;
        CloseContextFile(handle);
    }
    {
        // 6 pairs written, pair index 1 deliberately mismatched -> 5 aligned pairs
        // with sequence ids 0, 2, 3, 4, 5 (matches WriteSyntheticContextFile's own
        // documented mismatch-at-index-1 behavior, same fixture Task 6/7 already use).
        WriteSyntheticContextFile("/tmp/ctx_reader_test_resume.context", 6);
        auto handle = OpenContextFile("/tmp/ctx_reader_test_resume.context");

        CollectedPairs full_pass;
        auto [full_counters, full_resume] = ReadAllRecordsResumable(
            handle, handle.record_stream_offset, 0, false, CollectPair, &full_pass);
        (void)full_counters;
        check("full resumable pass from the true start collects all 5 aligned pairs",
              full_pass.log_scale_ratios.size() == 5);
        check("resume_offset after a full pass equals file size", full_resume.resume_offset == handle.size);
        check("last_seq_id after a full pass is the final aligned pair's sequence_id (5)",
              full_resume.has_last_seq_id && full_resume.last_seq_id == 5);

        // Manually walk forward exactly 6 raw records (pairs 0, 1, 2) to compute a
        // genuine mid-stream resume offset -- the same way context_to_parquet.cpp's
        // own per-chunk loop derives its next call's start_offset from a PREVIOUS
        // call's returned ResumePoint, rather than assuming any particular value.
        std::size_t mid_offset = handle.record_stream_offset;
        for (int i = 0; i < 6; ++i) {
            std::uint32_t sz = 0;
            std::memcpy(&sz, handle.base + mid_offset, sizeof(sz));
            mid_offset += 4 + sz;
        }

        CollectedPairs resumed;
        auto [resumed_counters, resumed_resume] = ReadAllRecordsResumable(
            handle, mid_offset, /*start_last_seq_id=*/2, /*has_start_last_seq_id=*/true,
            CollectPair, &resumed);
        (void)resumed_resume;
        check("resuming mid-stream after 3 raw pairs collects exactly the remaining 3 aligned pairs",
              resumed.log_scale_ratios.size() == 3 &&
              resumed.log_scale_ratios[0] == 3.0f &&
              resumed.log_scale_ratios[1] == 4.0f &&
              resumed.log_scale_ratios[2] == 5.0f);
        check("no false sequence_regressions when resuming with the real last-seen seq_id (2)",
              resumed_counters.sequence_regressions == 0);
        CloseContextFile(handle);
    }
    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
