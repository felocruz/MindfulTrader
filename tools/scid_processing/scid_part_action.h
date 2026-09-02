// tools/scid_processing/scid_part_action.h
// Per-contract incremental-decode decision logic for scid_to_ticks_parquet.cpp,
// factored into a small pure function so it's testable without linking Arrow
// (see docs/superpowers/plans/2026-09-02-scid-tick-parquet-cpp-tool.md Task 6).
#ifndef TOOLS_SCID_PART_ACTION_H
#define TOOLS_SCID_PART_ACTION_H

#include "scid_mirror_sync.h"

namespace scid {

enum class PartAction { kFullDecode, kResumeAppend, kTrim, kSkip };

// full_rebuild or no existing part -> always a full decode (nothing to resume
// from). Otherwise: kNew/kShrunkOrChanged -> full decode (a shrink/replace
// means any existing part can't be trusted as a valid prefix); kGrown ->
// resume-append from the part's own last timestamp; kUnchanged -> trim if
// this contract's window has since closed (needs_trim, computed by the
// caller by comparing the existing part's max timestamp_us against the
// window's active_end_us) or skip entirely otherwise.
inline PartAction DecidePartAction(MirrorSyncAction sync_action, bool has_existing_part,
                                    bool full_rebuild, bool needs_trim) {
    if (full_rebuild || !has_existing_part) return PartAction::kFullDecode;
    if (sync_action == MirrorSyncAction::kNew || sync_action == MirrorSyncAction::kShrunkOrChanged) {
        return PartAction::kFullDecode;
    }
    if (sync_action == MirrorSyncAction::kGrown) {
        return PartAction::kResumeAppend;
    }
    return needs_trim ? PartAction::kTrim : PartAction::kSkip;
}

}  // namespace scid

#endif  // TOOLS_SCID_PART_ACTION_H
