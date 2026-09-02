// tools/scid_processing/scid_mirror_sync.h
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

// Cheap suffix check (last n bytes) -- the head-only prefix check cannot
// distinguish "genuinely unchanged" from "same size but content replaced"
// for a same-size file; combined with ReadPrefix, this is the cross-check
// used when a same-size file's mtime comparison can't be trusted (see
// SyncScidMirror's own comment on this -- verified empirically: WSL's 9p
// mount for a live Sierra Chart directory can report a live file's mtime
// as unequal to an already-synced, byte-identical mirror copy, even though
// `stat` shows nanosecond-identical timestamps on both sides).
inline std::vector<char> ReadSuffix(const std::filesystem::path& path, std::size_t n) {
    std::ifstream in(path, std::ios::binary);
    in.seekg(0, std::ios::end);
    const auto tell = in.tellg();
    const std::size_t file_size = tell > 0 ? static_cast<std::size_t>(tell) : 0;
    const std::size_t read_len = std::min(n, file_size);
    in.seekg(static_cast<std::streamoff>(file_size - read_len), std::ios::beg);
    std::vector<char> buf(read_len, 0);
    in.read(buf.data(), static_cast<std::streamsize>(read_len));
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

        if (live_size == mirror_size) {
            // mtime alone disagreed (a cross-filesystem WSL/9p quirk, verified
            // empirically -- not something we can fix at this layer) but the
            // size didn't change. A full multi-GB re-copy is expensive on a
            // slow live mount, so confirm real equality cheaply first: both
            // ends of the file must match (head-only isn't enough to catch a
            // same-size content replacement). Only a genuine mismatch forces
            // the full copy.
            const bool head_matches =
                detail::ReadPrefix(mirror_path, kPrefixLen) == detail::ReadPrefix(live_path, kPrefixLen);
            const bool tail_matches =
                detail::ReadSuffix(mirror_path, kPrefixLen) == detail::ReadSuffix(live_path, kPrefixLen);
            if (head_matches && tail_matches) {
                results.push_back({name, MirrorSyncAction::kUnchanged});
                continue;
            }
            std::fprintf(stderr,
                         "scid_mirror_sync: %s is the same size as its mirror copy but its content "
                         "differs (head_matches=%d tail_matches=%d) -- forcing a full re-decode of "
                         "this contract, not a delta.\n",
                         name.c_str(), head_matches, tail_matches);
            detail::AtomicCopy(live_path, mirror_path);
            results.push_back({name, MirrorSyncAction::kShrunkOrChanged});
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
