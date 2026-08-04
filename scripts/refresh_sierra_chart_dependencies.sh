#!/usr/bin/env bash
################################################################################
# Refresh sierra_chart_dependencies/ from a live Sierra Chart installation.
#
# WHY THIS SCRIPT EXISTS: sierra_chart_dependencies/ is a curated, git-tracked
# vendor snapshot of the Sierra Chart ACSIL SDK -- the *authoritative* build
# dependency for this project (see CLAUDE.md). It is deliberately NOT a
# symlink or live path into a Sierra Chart installation, because ACSIL is a
# raw C-ABI plugin interface: struct layouts here must match whatever build
# actually loads MindfulTrader.dll, and a live install auto-updates outside
# this repo's history. Confirmed drift class: s_VolumeAtPriceV2::Volume was
# `unsigned int` in the pinned copy and `double` in a newer live install --
# a silent behavioral difference that only a real rebuild caught.
#
# This script is the ONLY sanctioned way to update the vendored copy. Never
# hand-copy files from a live installation directly -- that skips the diff
# review and build gate below.
#
# USAGE:
#   scripts/refresh_sierra_chart_dependencies.sh [SOURCE_DIR]
#
#   SOURCE_DIR defaults to /mnt/c/SierraChart2/ACS_Source. Pass an explicit
#   path if your Sierra Chart installation lives elsewhere.
#
# WHAT IT DOES:
#   1. Prints a diff summary between the current vendored copy and SOURCE_DIR,
#      restricted to the files this project actually vendors (never blindly
#      pulls in SOURCE_DIR's example .cpp files).
#   2. Asks for confirmation before copying anything.
#   3. Copies the changed files and updates sierra_chart_dependencies/VERSION
#      from SOURCE_DIR/../VersionNumber.txt (if present).
#
# WHAT YOU MUST DO AFTER RUNNING IT (this script does not do these for you):
#   1. Review the diff of every changed header -- a struct field type change
#      is exactly the kind of thing that compiles fine in some call sites and
#      silently misbehaves in others.
#   2. Run ./build_dll.sh (full rebuild, not --no-clean) and confirm it's
#      still green.
#   3. Commit the refresh as its OWN isolated commit, never bundled with an
#      unrelated feature change -- e.g.:
#        git commit -m "chore: update vendored Sierra Chart SDK headers <old> -> <new>"
################################################################################
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TARGET_DIR="$REPO_ROOT/sierra_chart_dependencies"
SOURCE_DIR="${1:-/mnt/c/SierraChart2/ACS_Source}"

if [[ ! -d "$SOURCE_DIR" ]]; then
    echo "ERROR: source directory not found: $SOURCE_DIR" >&2
    echo "Pass the correct path as an argument if your Sierra Chart installation lives elsewhere." >&2
    exit 1
fi

echo "Source: $SOURCE_DIR"
echo "Target: $TARGET_DIR"
echo

echo "--- Files that differ (vendored file list only, not all of SOURCE_DIR) ---"
changed_files=()
for f in "$TARGET_DIR"/*; do
    name="$(basename "$f")"
    [[ "$name" == "VERSION" ]] && continue
    src="$SOURCE_DIR/$name"
    if [[ ! -f "$src" ]]; then
        echo "  MISSING in source: $name (leaving vendored copy untouched)"
        continue
    fi
    if ! cmp -s "$f" "$src"; then
        echo "  CHANGED: $name"
        changed_files+=("$name")
    fi
done

if [[ ${#changed_files[@]} -eq 0 ]]; then
    echo
    echo "No differences found. Nothing to refresh."
    exit 0
fi

echo
read -r -p "Copy the ${#changed_files[@]} changed file(s) into $TARGET_DIR? [y/N] " confirm
if [[ "$confirm" != "y" && "$confirm" != "Y" ]]; then
    echo "Aborted. No files were changed."
    exit 0
fi

for name in "${changed_files[@]}"; do
    cp "$SOURCE_DIR/$name" "$TARGET_DIR/$name"
    echo "  copied: $name"
done

version_file="$SOURCE_DIR/../VersionNumber.txt"
if [[ -f "$version_file" ]]; then
    build_number="$(cat "$version_file")"
    cat > "$TARGET_DIR/VERSION" <<EOF
$build_number

Copied from $SOURCE_DIR on $(date '+%Y-%m-%d') via scripts/refresh_sierra_chart_dependencies.sh.
EOF
    echo
    echo "Updated VERSION marker to build $build_number."
else
    echo
    echo "WARNING: no VersionNumber.txt found at $version_file -- VERSION marker left unchanged." >&2
    echo "Update sierra_chart_dependencies/VERSION by hand with whatever build/date info you have." >&2
fi

echo
echo "Next steps (required, not automated by this script):"
echo "  1. Review each changed header's diff -- look for struct field type/order changes,"
echo "     not just 'does it compile'."
echo "  2. Run ./build_dll.sh (full rebuild) and confirm it's green."
echo "  3. Run the full test suite (tests/cpp/*.cpp, tests/run_python_tests.sh)."
echo "  4. Commit as an isolated commit -- do not bundle with an unrelated feature change."
