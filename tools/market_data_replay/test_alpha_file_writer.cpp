// test_alpha_file_writer.cpp -- native round-trip test for AlphaFileWriter.h
// (Task 8 Step 2, docs/superpowers/plans/2026-09-16-market-data-replay-alpha-
// generator-implementation.md). No GoogleTest/CMake, matches this tool
// family's own check()/g_failures/ALL PASS harness convention.
//
// Build & run natively:
//   g++ -std=c++17 -Iinclude -Iinclude/generated -Itools/market_data_replay \
//       tools/market_data_replay/test_alpha_file_writer.cpp -o /tmp/test_afw && /tmp/test_afw

#include "AlphaFileWriter.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

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

// Minimal standalone reader (spec's own "or a new minimal reader" alternative
// to tools/context_pipeline/context_reader.h, which is `.context`-MO+SS-
// specific and doesn't apply here): reads the "LBRN" magic header, skips the
// size-prefixed FileMetadata record, then returns the bytes of the first
// size-prefixed TrainingEvent record.
std::vector<uint8_t> ReadFirstTrainingEventRecord(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    char magic[4] = {};
    in.read(magic, 4);
    if (!in || magic[0] != 'L' || magic[1] != 'B' || magic[2] != 'R' || magic[3] != 'N') {
        return {};
    }
    uint32_t metadataSize = 0;
    in.read(reinterpret_cast<char*>(&metadataSize), sizeof(metadataSize));
    in.seekg(metadataSize, std::ios::cur);  // skip FileMetadata payload

    uint32_t recordSize = 0;
    in.read(reinterpret_cast<char*>(&recordSize), sizeof(recordSize));
    if (!in) return {};
    std::vector<uint8_t> buf(recordSize);
    in.read(reinterpret_cast<char*>(buf.data()), recordSize);
    if (!in) return {};
    return buf;
}

}  // namespace

int main() {
    std::printf("AlphaFileWriter unit tests (Task 8)\n");

    const std::string kTestPath = "/tmp/test_alpha_file_writer_output";

    // Write one record with known, hand-chosen values across several scalar
    // fields (not sequence_id -- the writer owns and overwrites that field,
    // matching production's own "caller-supplied event, writer assigns the
    // sequence" contract, verified separately below).
    {
        AlphaFileWriter writer;
        check("open_succeeds", writer.Open(kTestPath, "MES"));

        MTS::Training::TrainingEventT event;
        event.bar_index = 42;
        event.timestamp_us = 1705359600123456LL;
        event.open = 100.25f;
        event.high = 101.5f;
        event.low = 99.75f;
        event.close = 100.5f;
        event.volume = 12345;
        event.hmm_state = 2;
        writer.LogAlpha(event);
        check("sequence_id_assigned_starting_at_zero", event.sequence_id == 0);

        writer.Close();
    }

    // Read the record back and confirm every hand-set field round-trips
    // byte-faithfully.
    {
        const std::vector<uint8_t> buf = ReadFirstTrainingEventRecord(kTestPath + ".alpha");
        check("record_read_back_non_empty", !buf.empty());

        const MTS::Training::TrainingEvent* readEvent =
            flatbuffers::GetRoot<MTS::Training::TrainingEvent>(buf.data());
        check("bar_index_round_trips", readEvent->bar_index() == 42);
        check("timestamp_us_round_trips", readEvent->timestamp_us() == 1705359600123456LL);
        check("open_round_trips", readEvent->open() == 100.25f);
        check("high_round_trips", readEvent->high() == 101.5f);
        check("low_round_trips", readEvent->low() == 99.75f);
        check("close_round_trips", readEvent->close() == 100.5f);
        check("volume_round_trips", readEvent->volume() == 12345);
        check("hmm_state_round_trips", readEvent->hmm_state() == 2);
        check("sequence_id_round_trips_as_zero", readEvent->sequence_id() == 0);
    }

    // A second record in the same run must get sequence_id=1 (monotonic,
    // matching production's own m_sequenceId++/m_globalSequenceId++ contract).
    {
        AlphaFileWriter writer;
        writer.Open(kTestPath, "MES");
        MTS::Training::TrainingEventT event1;
        MTS::Training::TrainingEventT event2;
        writer.LogAlpha(event1);
        writer.LogAlpha(event2);
        check("sequence_id_increments_monotonically", event1.sequence_id == 0 && event2.sequence_id == 1);
        writer.Close();
    }

    // LogAlpha() before Open() (or after Close()) is a documented no-op, not
    // a crash -- matches LBRFileManager::LogAlphaUnlocked()'s own
    // `if (!m_isOpen || !stream.is_open()) return;` guard.
    {
        AlphaFileWriter writer;
        MTS::Training::TrainingEventT event;
        event.sequence_id = 999;
        writer.LogAlpha(event);  // never opened
        check("log_alpha_before_open_is_a_safe_no_op", event.sequence_id == 999);
    }

    std::remove((kTestPath + ".alpha").c_str());

    std::printf("%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return g_failures == 0 ? 0 : 1;
}
