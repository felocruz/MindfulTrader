// tools/scid_processing/test_scid_reader.cpp
// Build & run: mamba run -n mts g++ -std=c++17 tools/scid_processing/test_scid_reader.cpp \
//   -o /tmp/test_scid_reader && /tmp/test_scid_reader
#include "scid_reader.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {
int g_failures = 0;
void check(const char* name, bool ok) {
    if (ok) { std::printf("  PASS  %s\n", name); }
    else { ++g_failures; std::printf("  FAIL  %s\n", name); }
}

// Writes a synthetic .scid file: a 56-byte header (contents irrelevant to
// the reader, only its size is load-bearing) followed by one 40-byte
// scid::ScidRecordView per entry in `records`.
void WriteScidFixture(const std::string& path, const std::vector<scid::ScidRecordView>& records) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    char header[scid::kHeaderSize] = {};
    out.write(header, sizeof(header));
    for (const auto& rec : records) {
        out.write(reinterpret_cast<const char*>(&rec), sizeof(rec));
    }
}

scid::ScidRecordView MakeRecord(std::int64_t raw_timestamp, float close, float high, float low,
                                 std::uint32_t volume, std::uint32_t bid_volume,
                                 std::uint32_t ask_volume) {
    scid::ScidRecordView rec{};
    rec.raw_timestamp = raw_timestamp;
    rec.open = close;
    rec.high = high;
    rec.low = low;
    rec.close = close;
    rec.trades = 1;
    rec.volume = volume;
    rec.bid_volume = bid_volume;
    rec.ask_volume = ask_volume;
    return rec;
}
}  // namespace

int main() {
    // -- ScidFileView open + record count --
    {
        const std::string path = "/tmp/scid_fixture_basic.scid";
        std::vector<scid::ScidRecordView> records;
        for (int i = 0; i < 10; ++i) {
            records.push_back(MakeRecord(scid::kScEpochOffsetUs + 1000 * i, 100.0f + i,
                                          100.5f + i, 99.5f + i, 5, 2, 3));
        }
        WriteScidFixture(path, records);
        scid::ScidFileView file(path);
        check("record count matches (file_size - header) / 40", file.RecordCount() == 10);
    }
    // -- Trailing bytes not a whole record throws --
    {
        const std::string path = "/tmp/scid_fixture_truncated.scid";
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        char header[scid::kHeaderSize] = {};
        out.write(header, sizeof(header));
        char partial[20] = {};
        out.write(partial, sizeof(partial));
        out.close();
        bool threw = false;
        try {
            scid::ScidFileView file(path);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        check("file with a non-whole trailing record throws", threw);
    }
    // -- LowerBoundUs --
    {
        const std::string path = "/tmp/scid_fixture_lowerbound.scid";
        std::vector<scid::ScidRecordView> records;
        // POSIX-us timestamps 0, 1000, 2000, ..., 9000 (raw_timestamp = posix_us + offset).
        for (int i = 0; i < 10; ++i) {
            records.push_back(MakeRecord(scid::kScEpochOffsetUs + 1000 * i, 100.0f, 100.0f,
                                          100.0f, 1, 0, 1));
        }
        WriteScidFixture(path, records);
        scid::ScidFileView file(path);
        check("LowerBoundUs exact match returns that index", file.LowerBoundUs(3000) == 3);
        check("LowerBoundUs strictly between two records returns the next index",
              file.LowerBoundUs(3500) == 4);
        check("LowerBoundUs before the first record returns 0", file.LowerBoundUs(-1000) == 0);
        check("LowerBoundUs after the last record returns record count",
              file.LowerBoundUs(100000) == 10);
    }
    // -- DecodeScidTicks: in-order fixture, exact field derivation --
    {
        const std::string path = "/tmp/scid_fixture_decode.scid";
        std::vector<scid::ScidRecordView> records = {
            MakeRecord(scid::kScEpochOffsetUs + 100, 10.0f, 10.25f, 9.75f, 5, 0, 3),  // ask side
            MakeRecord(scid::kScEpochOffsetUs + 200, 11.0f, 11.25f, 10.75f, 7, 4, 0),  // bid side
            MakeRecord(scid::kScEpochOffsetUs + 300, 12.0f, 12.25f, 11.75f, 2, 0, 0),  // neither
        };
        WriteScidFixture(path, records);
        scid::ScidFileView file(path);
        scid::DecodedTickChunk chunk;
        chunk.Reserve(16);
        scid::DecodedTickChunk collected;
        scid::DecodeScidTicks(file, 0, std::nullopt, /*chunk_rows=*/16, chunk,
                               [&](const scid::DecodedTickChunk& c) {
                                   collected.timestamp_us.insert(collected.timestamp_us.end(),
                                                                  c.timestamp_us.begin(), c.timestamp_us.end());
                                   collected.trade_price.insert(collected.trade_price.end(),
                                                                 c.trade_price.begin(), c.trade_price.end());
                                   collected.ask_price.insert(collected.ask_price.end(),
                                                               c.ask_price.begin(), c.ask_price.end());
                                   collected.bid_price.insert(collected.bid_price.end(),
                                                               c.bid_price.begin(), c.bid_price.end());
                                   collected.spread.insert(collected.spread.end(),
                                                            c.spread.begin(), c.spread.end());
                                   collected.trade_side.insert(collected.trade_side.end(),
                                                                c.trade_side.begin(), c.trade_side.end());
                               });
        check("in-order decode returns all 3 rows", collected.Rows() == 3);
        check("timestamp_us is raw_timestamp - kScEpochOffsetUs",
              collected.timestamp_us[0] == 100 && collected.timestamp_us[1] == 200 &&
              collected.timestamp_us[2] == 300);
        check("ask_price == high, bid_price == low, spread == high-low",
              collected.ask_price[0] == 10.25 && collected.bid_price[0] == 9.75 &&
              collected.spread[0] == static_cast<double>(10.25f) - static_cast<double>(9.75f));
        check("trade_side: ask_volume>0 -> 1", collected.trade_side[0] == 1);
        check("trade_side: bid_volume>0 (ask_volume==0) -> -1", collected.trade_side[1] == -1);
        check("trade_side: neither -> 0", collected.trade_side[2] == 0);
    }
    // -- Benign sub-1s jitter resequences correctly, preserving ties' relative order --
    {
        const std::string path = "/tmp/scid_fixture_jitter.scid";
        const std::int64_t base = scid::kScEpochOffsetUs;
        std::vector<scid::ScidRecordView> records = {
            MakeRecord(base + 1000, 1.0f, 1.0f, 1.0f, 1, 0, 1),
            MakeRecord(base + 2000, 2.0f, 2.0f, 2.0f, 1, 0, 1),
            MakeRecord(base + 1500, 3.0f, 3.0f, 3.0f, 1, 0, 1),  // 500us benign backward jump
            MakeRecord(base + 1500, 4.0f, 4.0f, 4.0f, 1, 0, 1),  // exact tie, stable order matters
            MakeRecord(base + 3000, 5.0f, 5.0f, 5.0f, 1, 0, 1),
        };
        WriteScidFixture(path, records);
        scid::ScidFileView file(path);
        scid::DecodedTickChunk chunk;
        chunk.Reserve(16);
        std::vector<double> prices;
        scid::DecodeScidTicks(file, 0, std::nullopt, 16, chunk,
                               [&](const scid::DecodedTickChunk& c) {
                                   prices.insert(prices.end(), c.trade_price.begin(), c.trade_price.end());
                               });
        check("jitter resequenced into chronological order with stable ties",
              prices.size() == 5 && prices[0] == 1.0 && prices[1] == 3.0 && prices[2] == 4.0 &&
              prices[3] == 2.0 && prices[4] == 5.0);
    }
    // -- Jump exceeding the benign bound throws --
    {
        const std::string path = "/tmp/scid_fixture_violation.scid";
        const std::int64_t base = scid::kScEpochOffsetUs;
        std::vector<scid::ScidRecordView> records = {
            MakeRecord(base + 5'000'000, 1.0f, 1.0f, 1.0f, 1, 0, 1),
            MakeRecord(base + 1'000'000, 2.0f, 2.0f, 2.0f, 1, 0, 1),  // 4,000,000us backward jump
        };
        WriteScidFixture(path, records);
        scid::ScidFileView file(path);
        scid::DecodedTickChunk chunk;
        chunk.Reserve(16);
        bool threw = false;
        try {
            scid::DecodeScidTicks(file, 0, std::nullopt, 16, chunk,
                                   [](const scid::DecodedTickChunk&) {});
        } catch (const std::runtime_error&) {
            threw = true;
        }
        check("a backward jump exceeding kMaxBenignTimestampJitterUs throws", threw);
    }
    // -- Chunking: 7 records, chunk_rows=2 -> 4 invocations (2,2,2,1), same chunk instance reused --
    {
        const std::string path = "/tmp/scid_fixture_chunking.scid";
        const std::int64_t base = scid::kScEpochOffsetUs;
        std::vector<scid::ScidRecordView> records;
        for (int i = 0; i < 7; ++i) {
            records.push_back(MakeRecord(base + 1000 * i, static_cast<float>(i), 1.0f, 1.0f, 1, 0, 1));
        }
        WriteScidFixture(path, records);
        scid::ScidFileView file(path);
        scid::DecodedTickChunk chunk;
        chunk.Reserve(2);
        int n_invocations = 0;
        std::vector<std::size_t> row_counts;
        std::vector<double> concatenated;
        const scid::DecodedTickChunk* chunk_addr = &chunk;
        bool same_instance_every_time = true;
        scid::DecodeScidTicks(file, 0, std::nullopt, /*chunk_rows=*/2, chunk,
                               [&](const scid::DecodedTickChunk& c) {
                                   ++n_invocations;
                                   if (&c != chunk_addr) same_instance_every_time = false;
                                   row_counts.push_back(c.Rows());
                                   concatenated.insert(concatenated.end(), c.trade_price.begin(),
                                                        c.trade_price.end());
                               });
        check("7 records at chunk_rows=2 invokes on_chunk exactly 4 times", n_invocations == 4);
        check("per-chunk row counts are 2,2,2,1",
              row_counts.size() == 4 && row_counts[0] == 2 && row_counts[1] == 2 &&
              row_counts[2] == 2 && row_counts[3] == 1);
        check("same DecodedTickChunk instance/address reused across every invocation",
              same_instance_every_time);
        check("concatenated chunk order matches original record order",
              concatenated.size() == 7 && concatenated[0] == 0.0 && concatenated[6] == 6.0);
    }
    // -- AdviseRange is callable without error and doesn't affect decode correctness --
    {
        const std::string path = "/tmp/scid_fixture_advise.scid";
        std::vector<scid::ScidRecordView> records = {
            MakeRecord(scid::kScEpochOffsetUs + 100, 1.0f, 1.0f, 1.0f, 1, 0, 1),
            MakeRecord(scid::kScEpochOffsetUs + 200, 2.0f, 2.0f, 2.0f, 1, 0, 1),
        };
        WriteScidFixture(path, records);
        scid::ScidFileView file(path);
        file.AdviseRange(0, file.RecordCount(), MADV_SEQUENTIAL);
        file.AdviseRange(0, file.RecordCount(), MADV_DONTNEED);
        scid::DecodedTickChunk chunk;
        chunk.Reserve(16);
        std::size_t rows = 0;
        scid::DecodeScidTicks(file, 0, std::nullopt, 16, chunk,
                               [&](const scid::DecodedTickChunk& c) { rows += c.Rows(); });
        check("decode still produces correct output with madvise calls wired in", rows == 2);
    }

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
