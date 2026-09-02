// tools/test_scid_mirror_sync.cpp
// Build & run: mamba run -n mts g++ -std=c++17 tools/test_scid_mirror_sync.cpp \
//   -o /tmp/test_scid_mirror_sync && /tmp/test_scid_mirror_sync
#include "scid_mirror_sync.h"

#include <chrono>
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

void WriteFile(const std::filesystem::path& p, const std::string& content) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::string ReadFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void SetMtime(const std::filesystem::path& p, std::filesystem::file_time_type t) {
    std::filesystem::last_write_time(p, t);
}

std::string MakePattern(std::size_t n, char base) {
    std::string s(n, base);
    for (std::size_t i = 0; i < n; ++i) s[i] = static_cast<char>(base + static_cast<char>(i % 7));
    return s;
}

void CleanDir(const std::filesystem::path& dir) {
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
}
}  // namespace

int main() {
    const std::filesystem::path live_dir = "/tmp/scid_mirror_sync_live";
    const std::filesystem::path mirror_dir = "/tmp/scid_mirror_sync_mirror";

    // -- New file: copied in, byte-identical afterward --
    {
        CleanDir(live_dir);
        CleanDir(mirror_dir);
        const std::string content = MakePattern(200, 'A');
        WriteFile(live_dir / "MESU23-CME.scid", content);

        auto results = scid::SyncScidMirror(live_dir.string(), mirror_dir.string());
        check("new file: exactly one result", results.size() == 1);
        check("new file: action is kNew",
              !results.empty() && results[0].action == scid::MirrorSyncAction::kNew);
        check("new file: mirror copy is byte-identical to live",
              ReadFile(mirror_dir / "MESU23-CME.scid") == content);
    }

    // -- Unchanged file (same size/mtime): skipped, sentinel byte untouched --
    {
        CleanDir(live_dir);
        CleanDir(mirror_dir);
        const std::string live_content = MakePattern(150, 'B');
        std::string mirror_content = live_content;
        mirror_content[10] = static_cast<char>(0x7F);  // sentinel: would be overwritten by a real copy
        WriteFile(live_dir / "MESU24-CME.scid", live_content);
        WriteFile(mirror_dir / "MESU24-CME.scid", mirror_content);
        const auto t = std::filesystem::file_time_type::clock::now();
        SetMtime(live_dir / "MESU24-CME.scid", t);
        SetMtime(mirror_dir / "MESU24-CME.scid", t);

        auto results = scid::SyncScidMirror(live_dir.string(), mirror_dir.string());
        check("unchanged file: action is kUnchanged",
              !results.empty() && results[0].action == scid::MirrorSyncAction::kUnchanged);
        check("unchanged file: sentinel byte survives (no real copy happened)",
              ReadFile(mirror_dir / "MESU24-CME.scid") == mirror_content);
    }

    // -- Grown file (mirror is a true byte-prefix of live): kGrown, mirror now matches live --
    {
        CleanDir(live_dir);
        CleanDir(mirror_dir);
        const std::string live_content = MakePattern(300, 'C');
        const std::string mirror_content = live_content.substr(0, 150);
        WriteFile(live_dir / "MESZ23-CME.scid", live_content);
        WriteFile(mirror_dir / "MESZ23-CME.scid", mirror_content);

        auto results = scid::SyncScidMirror(live_dir.string(), mirror_dir.string());
        check("grown file: action is kGrown",
              !results.empty() && results[0].action == scid::MirrorSyncAction::kGrown);
        check("grown file: mirror now matches live exactly",
              ReadFile(mirror_dir / "MESZ23-CME.scid") == live_content);
    }

    // -- Shrunk file: kShrunkOrChanged, mirror now matches the smaller live file, warning logged --
    {
        CleanDir(live_dir);
        CleanDir(mirror_dir);
        const std::string bigger_content = MakePattern(300, 'D');
        const std::string smaller_content = bigger_content.substr(0, 100);
        WriteFile(live_dir / "MESZ24-CME.scid", smaller_content);   // live re-exported, now smaller
        WriteFile(mirror_dir / "MESZ24-CME.scid", bigger_content);  // stale, bigger mirror

        std::FILE* captured = std::freopen("/tmp/scid_mirror_sync_stderr.txt", "w", stderr);
        auto results = scid::SyncScidMirror(live_dir.string(), mirror_dir.string());
        std::fflush(stderr);
        (void)captured;
        check("shrunk file: action is kShrunkOrChanged",
              !results.empty() && results[0].action == scid::MirrorSyncAction::kShrunkOrChanged);
        check("shrunk file: mirror now matches the smaller live file",
              ReadFile(mirror_dir / "MESZ24-CME.scid") == smaller_content);
        const std::string logged = ReadFile("/tmp/scid_mirror_sync_stderr.txt");
        check("shrunk file: a warning was logged", logged.find("MESZ24-CME.scid") != std::string::npos);
    }

    // -- Header-bytes-changed-but-same-size: kShrunkOrChanged --
    {
        CleanDir(live_dir);
        CleanDir(mirror_dir);
        const std::string live_content = MakePattern(200, 'E');
        const std::string mirror_content = MakePattern(200, 'F');  // same length, different bytes throughout
        WriteFile(live_dir / "MESH24-CME.scid", live_content);
        WriteFile(mirror_dir / "MESH24-CME.scid", mirror_content);
        SetMtime(live_dir / "MESH24-CME.scid", std::filesystem::file_time_type::clock::now());
        SetMtime(mirror_dir / "MESH24-CME.scid",
                  std::filesystem::file_time_type::clock::now() - std::chrono::hours(1));

        auto results = scid::SyncScidMirror(live_dir.string(), mirror_dir.string());
        check("header-changed-same-size file: action is kShrunkOrChanged",
              !results.empty() && results[0].action == scid::MirrorSyncAction::kShrunkOrChanged);
        check("header-changed-same-size file: mirror now matches live",
              ReadFile(mirror_dir / "MESH24-CME.scid") == live_content);
    }

    // -- Simulated interrupted copy: prior good mirror file is untouched by a stray .tmp --
    {
        CleanDir(live_dir);
        CleanDir(mirror_dir);
        const std::string content = MakePattern(120, 'G');
        WriteFile(live_dir / "MESM24-CME.scid", content);
        WriteFile(mirror_dir / "MESM24-CME.scid", content);  // already in sync
        const auto t = std::filesystem::file_time_type::clock::now();
        SetMtime(live_dir / "MESM24-CME.scid", t);
        SetMtime(mirror_dir / "MESM24-CME.scid", t);
        WriteFile(mirror_dir / "MESM24-CME.scid.tmp", "garbage-from-a-killed-previous-copy");

        auto results = scid::SyncScidMirror(live_dir.string(), mirror_dir.string());
        check("interrupted-copy simulation: real file is treated as kUnchanged",
              !results.empty() && results[0].action == scid::MirrorSyncAction::kUnchanged);
        check("interrupted-copy simulation: the real filename's content is never torn",
              ReadFile(mirror_dir / "MESM24-CME.scid") == content);
    }

    // -- Non-matching filenames are ignored entirely --
    {
        CleanDir(live_dir);
        CleanDir(mirror_dir);
        WriteFile(live_dir / "MES-202409-CME-USD.scid", "old-convention-stub");
        WriteFile(live_dir / "readme.txt", "not a contract file");

        auto results = scid::SyncScidMirror(live_dir.string(), mirror_dir.string());
        check("non-matching filenames produce zero results", results.empty());
        check("non-matching filenames are never copied into the mirror",
              !std::filesystem::exists(mirror_dir / "MES-202409-CME-USD.scid") &&
              !std::filesystem::exists(mirror_dir / "readme.txt"));
    }

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
