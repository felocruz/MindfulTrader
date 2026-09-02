// tools/tick_pipeline/scid_mirror_sync.h
// Refreshes the local .scid mirror (e.g. lbrnet/data/scid/) from the live
// Sierra Chart directory, incrementally. Ports lbrnet's own (currently
// unimplemented) design in docs/superpowers/specs/2026-08-18-mes-scid-
// incremental-sync-design.md to C++ -- see
// docs/superpowers/specs/2026-09-02-scid-tick-parquet-cpp-tool-spec.md §5.
//
// Read-only against live_dir. Copy is never in-place: write to
// "{name}.tmp" in the mirror directory, then atomically rename over the
// final name (POSIX rename on the same filesystem is atomic), so an
// interrupted copy (e.g. Sierra Chart still appending to the live file, or
// this process being killed mid-copy) never leaves the real filename torn.
// std::filesystem::copy_file dispatches to copy_file_range/sendfile on
// Linux glibc -- a kernel-side copy, never a hand-rolled userspace-buffer
// read/write loop.
#ifndef TOOLS_SCID_MIRROR_SYNC_H
#define TOOLS_SCID_MIRROR_SYNC_H

#include "scid_contract_windows.h"
#include "scid_reader.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace scid {

enum class MirrorSyncAction { kNew, kUnchanged, kGrown, kShrunkOrChanged };

struct MirrorSyncResult {
    std::string contract_filename;
    MirrorSyncAction action;
};

namespace detail {

// Cheap prefix check (header + first record) -- never a full re-read of a
// multi-GB file. A short read (e.g. the mirror file has fewer bytes than
// the requested prefix) naturally compares unequal to a full-length prefix,
// which is the safe default (treat as changed, force full reprocess).
inline std::vector<char> ReadPrefix(const std::filesystem::path& path, std::size_t n) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> buf(n, 0);
    in.read(buf.data(), static_cast<std::streamsize>(n));
    buf.resize(static_cast<std::size_t>(in.gcount() > 0 ? in.gcount() : 0));
    return buf;
}

inline void AtomicCopy(const std::filesystem::path& src, const std::filesystem::path& dst_final) {
    const std::filesystem::path tmp = dst_final;
    std::filesystem::path tmp_path = dst_final;
    tmp_path += ".tmp";
    std::filesystem::copy_file(src, tmp_path, std::filesystem::copy_options::overwrite_existing);
    std::filesystem::rename(tmp_path, dst_final);
    (void)tmp;
}

}  // namespace detail

// For every MES[HMUZ]\d{2}-CME\.scid file discovered under live_dir, syncs
// mirror_dir to match it (new/grown -> copy in; unchanged -> skip;
// shrunk/header-changed -> force full copy + warn) and reports what
// happened. Does not itself trigger any decode -- that is the CLI
// orchestration layer's job.
inline std::vector<MirrorSyncResult> SyncScidMirror(const std::string& live_dir,
                                                     const std::string& mirror_dir) {
    std::filesystem::create_directories(mirror_dir);
    std::vector<MirrorSyncResult> results;
    constexpr std::size_t kPrefixLen = kHeaderSize + sizeof(ScidRecordView);

    for (const auto& entry : std::filesystem::directory_iterator(live_dir)) {
        if (!entry.is_regular_file()) continue;
        const std::string name = entry.path().filename().string();
        if (!ParseContractFilename(name).has_value()) continue;

        const std::filesystem::path live_path = entry.path();
        const std::filesystem::path mirror_path = std::filesystem::path(mirror_dir) / name;

        if (!std::filesystem::exists(mirror_path)) {
            detail::AtomicCopy(live_path, mirror_path);
            results.push_back({name, MirrorSyncAction::kNew});
            continue;
        }

        const auto live_size = std::filesystem::file_size(live_path);
        const auto mirror_size = std::filesystem::file_size(mirror_path);
        const auto live_mtime = std::filesystem::last_write_time(live_path);
        const auto mirror_mtime = std::filesystem::last_write_time(mirror_path);

        if (live_size == mirror_size && live_mtime == mirror_mtime) {
            results.push_back({name, MirrorSyncAction::kUnchanged});
            continue;
        }

        const bool header_matches =
            detail::ReadPrefix(mirror_path, kPrefixLen) == detail::ReadPrefix(live_path, kPrefixLen);

        if (live_size < mirror_size || !header_matches) {
            std::fprintf(stderr,
                         "scid_mirror_sync: %s shrank or its header changed (mirror=%llu live=%llu "
                         "bytes) -- forcing a full re-decode of this contract, not a delta.\n",
                         name.c_str(), static_cast<unsigned long long>(mirror_size),
                         static_cast<unsigned long long>(live_size));
            detail::AtomicCopy(live_path, mirror_path);
            results.push_back({name, MirrorSyncAction::kShrunkOrChanged});
        } else {
            detail::AtomicCopy(live_path, mirror_path);
            results.push_back({name, MirrorSyncAction::kGrown});
        }
    }
    return results;
}

}  // namespace scid

#endif  // TOOLS_SCID_MIRROR_SYNC_H
