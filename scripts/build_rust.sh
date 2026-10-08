#!/bin/bash
# build_rust.sh [--features hmm,obs,transport] -- cross-builds rust/ffi (mts_ffi.lib) for Sierra.
# Called by build_dll.sh before CMake whenever MTS_WITH_RUST=ON; safe to run standalone too.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/rust_windows_env.sh"

FEATURES="${1:-}"
FEATURES="${FEATURES#--features }"

cd "$ROOT/rust"   # .cargo/config.toml + rust-toolchain.toml discovery
if [ -n "$FEATURES" ]; then
    cargo build -p mts_ffi --release --target x86_64-pc-windows-msvc --features "$FEATURES"
else
    cargo build -p mts_ffi --release --target x86_64-pc-windows-msvc
fi

GENERATED_HEADER="$ROOT/include/generated/mts_core.h"
if [ -f "$GENERATED_HEADER" ] && ! git -C "$ROOT" diff --quiet -- "$GENERATED_HEADER" 2>/dev/null; then
    echo "NOTE: $GENERATED_HEADER changed -- commit it alongside the Rust source that changed it."
fi
