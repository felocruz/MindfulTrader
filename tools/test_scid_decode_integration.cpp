// tools/test_scid_decode_integration.cpp
// Build & run: mamba run -n mts g++ -std=c++17 tools/test_scid_decode_integration.cpp \
//   -o /tmp/test_scid_decode_integration && /tmp/test_scid_decode_integration
//
// End-to-end check of Tasks 1+2 together against a small REAL contract file
// slice (first 5000 records = 200,056 bytes of the real MESU23-CME.scid) --
// not a multi-GB fixture, and not committed to the repo; copied at test time
// from the real local mirror.
#include "scid_contract_windows.h"
#include "scid_reader.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
int g_failures = 0;
void check(const char* name, bool ok) {
    if (ok) { std::printf("  PASS  %s\n", name); }
    else { ++g_failures; std::printf("  FAIL  %s\n", name); }
}

constexpr const char* kRealSourcePath = "/home/rcruz/devel/VSCode/lbrnet/data/scid/MESU23-CME.scid";
constexpr std::size_t kFixtureRecords = 5000;
constexpr std::size_t kFixtureBytes = scid::kHeaderSize + kFixtureRecords * sizeof(scid::ScidRecordView);

// Hand-verified via an independent Python struct.unpack pass over the real
// source file's first 5000 records (see this session's own verification --
// not asserted from memory).
constexpr std::int64_t kExpectedFirstTimestampUs = 1685916000062000;
constexpr std::int64_t kExpectedLastTimestampUs = 1685976411886001;
}  // namespace

int main() {
    if (!std::filesystem::exists(kRealSourcePath)) {
        std::printf("SKIP: real source file not found at %s (expected on this dev machine only)\n",
                    kRealSourcePath);
        return 0;
    }

    const std::filesystem::path dir = "/tmp/scid_decode_integration_fixture";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::filesystem::path fixture_path = dir / "MESU23-CME.scid";
    {
        std::ifstream src(kRealSourcePath, std::ios::binary);
        std::ofstream dst(fixture_path, std::ios::binary | std::ios::trunc);
        std::vector<char> buf(kFixtureBytes);
        src.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        check("real source file has at least kFixtureBytes available", src.gcount() == static_cast<std::streamsize>(kFixtureBytes));
        dst.write(buf.data(), static_cast<std::streamsize>(buf.size()));
    }

    scid::ScidFileView file(fixture_path.string());
    check("fixture record count matches the truncated byte count exactly", file.RecordCount() == kFixtureRecords);

    auto windows = scid::DiscoverContracts(dir.string());
    check("DiscoverContracts finds exactly one (open-ended) window for the single fixture file",
          windows.size() == 1 && !windows[0].active_end_us.has_value() && windows[0].active_start_us == 0);

    scid::DecodedTickChunk chunk;
    chunk.Reserve(kFixtureRecords);
    scid::DecodedTickChunk all;
    scid::DecodeScidTicks(file, windows[0].active_start_us, windows[0].active_end_us,
                           /*chunk_rows=*/kFixtureRecords, chunk,
                           [&](const scid::DecodedTickChunk& c) {
                               all.timestamp_us.insert(all.timestamp_us.end(), c.timestamp_us.begin(),
                                                        c.timestamp_us.end());
                               all.ask_price.insert(all.ask_price.end(), c.ask_price.begin(), c.ask_price.end());
                               all.bid_price.insert(all.bid_price.end(), c.bid_price.begin(), c.bid_price.end());
                               all.spread.insert(all.spread.end(), c.spread.begin(), c.spread.end());
                           });

    check("decoded row count matches the fixture's own record count", all.Rows() == kFixtureRecords);
    check("first timestamp_us matches the independently-verified real value",
          !all.timestamp_us.empty() && all.timestamp_us.front() == kExpectedFirstTimestampUs);
    check("last timestamp_us matches the independently-verified real value",
          !all.timestamp_us.empty() && all.timestamp_us.back() == kExpectedLastTimestampUs);

    bool all_spreads_non_negative = true;
    for (std::size_t i = 0; i < all.Rows(); ++i) {
        if (all.ask_price[i] < all.bid_price[i] || all.spread[i] < 0.0) {
            all_spreads_non_negative = false;
            break;
        }
    }
    check("ask_price >= bid_price (spread >= 0) for every real decoded row", all_spreads_non_negative);

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
