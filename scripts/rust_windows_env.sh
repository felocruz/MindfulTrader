#!/bin/bash
# rust_windows_env.sh -- cc-rs cross variables for x86_64-pc-windows-msvc, sourced (not executed) by
# scripts/build_rust.sh and by hand for ad-hoc `cargo build -p ... --target x86_64-pc-windows-msvc`.
# Pattern proven in ../Atratus/cpp/build_dll.sh and this repo's own rust_probe/W0s spikes.
XWIN_SYSROOT="${XWIN_SYSROOT:-$HOME/.local/sysroots/x86_64-pc-windows-msvc}"
export CC_x86_64_pc_windows_msvc=clang-cl-22
export CXX_x86_64_pc_windows_msvc=clang-cl-22
export AR_x86_64_pc_windows_msvc=/usr/lib/llvm-22/bin/llvm-lib
export CFLAGS_x86_64_pc_windows_msvc="--target=x86_64-pc-windows-msvc /winsysroot$XWIN_SYSROOT"
export CXXFLAGS_x86_64_pc_windows_msvc="$CFLAGS_x86_64_pc_windows_msvc"
# Why /winsysroot: the sysroot path has no spaces, unlike "Windows Kits", which cc-rs would split.
