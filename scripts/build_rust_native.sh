#!/bin/bash
# build_rust_native.sh -- builds the Linux-native rust/ffi static library
# (rust/target/release/libffi.a) that native C++ tests (tests/cpp) and standalone tools (tools/)
# must link: the observation-vector formula headers (SevcikFractalDimension.h, DfaHurstExponent.h,
# CarryForwardCalculators.h, ...) call into Rust (spec decision #42). Link with:
#   rust/target/release/libffi.a -lpthread -ldl -lm
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT/rust"
cargo build -p ffi --release --features observation_vector,student_t_hmm
echo "Built $ROOT/rust/target/release/libffi.a"
