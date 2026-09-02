// tools/tick_pipeline/test_scid_contract_windows.cpp
// Build & run: mamba run -n mts g++ -std=c++17 tools/tick_pipeline/test_scid_contract_windows.cpp \
//   -o /tmp/test_scid_contract_windows && /tmp/test_scid_contract_windows
#include "scid_contract_windows.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
int g_failures = 0;
void check(const char* name, bool ok) {
    if (ok) { std::printf("  PASS  %s\n", name); }
    else { ++g_failures; std::printf("  FAIL  %s\n", name); }
}

void TouchFile(const std::filesystem::path& path) {
    std::ofstream(path.string(), std::ios::binary | std::ios::trunc);
}

void CleanDir(const std::filesystem::path& dir) {
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
}
}  // namespace

int main() {
    // -- ThirdFriday / ContractActiveEnd against hand-checked real calendar months --
    // (independently verified: Jan 1 2024 = Monday, Jan 1 2025 = Wednesday, standard
    // Gregorian calendar day-of-week arithmetic -- see this file's own derivation.)
    check("3rd Friday of March 2024 is 2024-03-15",
          scid::ThirdFriday(2024, 3) == scid::CivilDate{2024, 3, 15});
    check("H24 active_end (4 days before) is 2024-03-11",
          scid::ContractActiveEnd('H', 2024) == scid::CivilDate{2024, 3, 11});
    check("3rd Friday of June 2024 is 2024-06-21",
          scid::ThirdFriday(2024, 6) == scid::CivilDate{2024, 6, 21});
    check("M24 active_end is 2024-06-17", scid::ContractActiveEnd('M', 2024) == scid::CivilDate{2024, 6, 17});
    check("3rd Friday of September 2024 is 2024-09-20",
          scid::ThirdFriday(2024, 9) == scid::CivilDate{2024, 9, 20});
    check("U24 active_end is 2024-09-16", scid::ContractActiveEnd('U', 2024) == scid::CivilDate{2024, 9, 16});
    check("3rd Friday of December 2024 is 2024-12-20",
          scid::ThirdFriday(2024, 12) == scid::CivilDate{2024, 12, 20});
    check("Z24 active_end is 2024-12-16", scid::ContractActiveEnd('Z', 2024) == scid::CivilDate{2024, 12, 16});
    check("3rd Friday of March 2025 (year boundary) is 2025-03-21",
          scid::ThirdFriday(2025, 3) == scid::CivilDate{2025, 3, 21});
    check("H25 active_end is 2025-03-17", scid::ContractActiveEnd('H', 2025) == scid::CivilDate{2025, 3, 17});

    // -- ParseContractFilename --
    {
        auto parsed = scid::ParseContractFilename("MESU23-CME.scid");
        check("parses MESU23-CME.scid -> ('U', 2023)",
              parsed.has_value() && parsed->first == 'U' && parsed->second == 2023);
    }
    check("rejects the old-convention stub filename (must NOT match)",
          !scid::ParseContractFilename("MES-202409-CME-USD.scid").has_value());
    check("rejects a wrong symbol prefix", !scid::ParseContractFilename("ESU23-CME.scid").has_value());
    check("rejects an unknown month code", !scid::ParseContractFilename("MESX23-CME.scid").has_value());

    // -- DiscoverContracts: full H/M/U/Z/H/M/U/Z cycle, non-overlapping windows --
    {
        const std::filesystem::path dir = "/tmp/scid_contracts_full_cycle";
        CleanDir(dir);
        for (const std::string& name :
             {"MESH23-CME.scid", "MESM23-CME.scid", "MESU23-CME.scid", "MESZ23-CME.scid",
              "MESH24-CME.scid", "MESM24-CME.scid", "MESU24-CME.scid", "MESZ24-CME.scid"}) {
            TouchFile(dir / name);
        }
        auto windows = scid::DiscoverContracts(dir.string());
        check("full 8-contract cycle returns 8 windows", windows.size() == 8);
        bool chained = true;
        for (std::size_t i = 1; i < windows.size(); ++i) {
            if (!windows[i - 1].active_end_us.has_value() ||
                windows[i].active_start_us != *windows[i - 1].active_end_us) {
                chained = false;
            }
        }
        check("windows chain start[i] == end[i-1] with no gaps/overlaps", chained);
        check("only the last (most recent) window is open-ended",
              !windows.back().active_end_us.has_value() &&
              std::all_of(windows.begin(), windows.end() - 1,
                          [](const scid::ContractWindow& w) { return w.active_end_us.has_value(); }));
        check("windows are sorted chronologically by contract",
              windows.front().month_code == 'H' && windows.front().year == 2023 &&
              windows.back().month_code == 'Z' && windows.back().year == 2024);
    }
    // -- DiscoverContracts: missing contract mid-cycle throws, names the missing contract --
    {
        const std::filesystem::path dir = "/tmp/scid_contracts_missing";
        CleanDir(dir);
        for (const std::string& name : {"MESH23-CME.scid", "MESM23-CME.scid", "MESZ23-CME.scid"}) {
            TouchFile(dir / name);  // U23 deliberately missing
        }
        bool threw = false;
        std::string message;
        try {
            scid::DiscoverContracts(dir.string());
        } catch (const std::invalid_argument& e) {
            threw = true;
            message = e.what();
        }
        check("missing mid-cycle contract throws", threw);
        check("error message names the missing contract (U23)", message.find("U23") != std::string::npos);
    }
    // -- DiscoverContracts: empty directory throws --
    {
        const std::filesystem::path dir = "/tmp/scid_contracts_empty";
        CleanDir(dir);
        bool threw = false;
        try {
            scid::DiscoverContracts(dir.string());
        } catch (const std::runtime_error&) {
            threw = true;
        }
        check("empty directory throws", threw);
    }
    // -- DiscoverContracts: only non-matching filenames throws --
    {
        const std::filesystem::path dir = "/tmp/scid_contracts_nonmatching";
        CleanDir(dir);
        TouchFile(dir / "MES-202409-CME-USD.scid");
        TouchFile(dir / "readme.txt");
        bool threw = false;
        try {
            scid::DiscoverContracts(dir.string());
        } catch (const std::runtime_error&) {
            threw = true;
        }
        check("directory with only non-matching filenames throws", threw);
    }

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
