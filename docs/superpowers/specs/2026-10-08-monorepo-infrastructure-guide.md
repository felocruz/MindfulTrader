# Monorepo Infrastructure Guide — Cargo, CMake, Python envs, JSON/TOML (a guide for Claude)

**Status:** guide only, nothing implemented. Written 2026-10-08 at the operator's request
("give a guide to yourself on how to set up the infrastructure — Cargo, the .toml and .json files,
CMake — so the four projects evolve into a single monolith"). Written from an Atratus session, after
surveying all four repos read-only (findings: §1).

**Read with:**
- `2026-10-07-mindfultrader-monorepo-consolidation-spec.md`, **the umbrella; it wins on any conflict.**
- `2026-10-07-rust-hmm-lifecycle-spec.md`.

This guide is the *how* for the umbrella spec's **W2 ("`rust/` workspace bootstrap")**. It also covers
the plumbing that pre-work P2/P3/P6 and merge Stages 3–5 need. It does not re-decide anything ruled
there: the layout, the sequencing, the HMM moving to Rust, and 5561 deleted.

**Atratus is a separate project** (umbrella §1). Never add a Cargo `path =` dependency, a symlink, or a
`sys.path` entry pointing into `../Atratus`. Where its code is a useful start (its HMM, its ZMQ ring,
its cross-build), **copy** it with a provenance comment and own it here.

---

## 0. The shape in one picture

```
VSCode/MindfulTrader/                      repo root = the monorepo
├── rust-toolchain.toml                    ← Rust pin; MUST be at the root (§3.1)
├── .gitignore  .gitattributes  .editorconfig  .pre-commit-config.yaml
├── MindfulTrader.code-workspace           ← multi-root VS Code workspace, one Python interpreter per folder
├── .github/workflows/ci.yml               ← one workflow, path-filtered jobs (§8)
├── scripts/                               ← build_rust.sh, install_py_ext.sh, check_all.sh, check_north_star.sh
├── docs/  PRODUCTION_TRIAGE.md  .claude/
├── schema/                                ← .fbs + regenerate_schema.sh (now also emits Rust, §5)
├── rust/                                  ← THE Cargo workspace (umbrella §8/§12.1)
│   ├── Cargo.toml  Cargo.lock  .cargo/config.toml
│   ├── schema/   mts_schema    generated FlatBuffers Rust + dim/contract constants   (pure)
│   ├── observation_vector/  mts_observation_vector  18 dims, FeatureScaler, Mahalanobis gate     (pure)
│   ├── hmm/      mts_hmm       Student-t HMM: train, filter, regime engine, artifacts (pure)
│   ├── transport/mts_transport every ZMQ socket                                       (zmq)
│   ├── ffi/      mts_ffi       ONE staticlib + cbindgen header → linked into the DLL  (§2.2)
│   ├── py/       mts_py        ONE PyO3 extension "mindful_core" → lbrnet + GUI envs  (§2.2)
│   └── tools/    mts_tools     bins: hmm_tool, obs_tool (train, posteriors, replay, verify-golden)
├── cpp/                                   ← the Sierra DLL (CMake, clang-cl, xwin); links rust/ffi
├── lbrnet/                                ← Python, own env + lock; imports mindful_core
└── GUI/                                   ← Python (Dash), own env + lock; imports mindful_core
```

**Build flow:**

```
schema/*.fbs ─flatc─► cpp/include/generated (C++) · lbrnet/lbrnet/generated (Py) · rust/schema/src/generated (Rust)
rust/ ─cargo (windows-msvc, release)─► mts_ffi.lib ─CMake link─► cpp/build-windows/bin/MindfulTrader.dll
rust/ ─maturin (linux, abi3)─► mindful_core-*.whl ─pip install─► lbrnet env, GUI env
```

---

## 1. Facts this guide is built on (surveyed 2026-10-08, read-only)

| Project | Build / env | What matters for the infra |
|---|---|---|
| **cpp** (MindfulTrader) | CMake 3.20, Ninja, preset `wsl-clang-cl-release`, `toolchain-clang-cl.cmake` → clang-cl-22 + xwin sysroot `~/.local/sysroots/x86_64-pc-windows-msvc` (CRT 14.44.17.14, SDK 10.0.26100); `build_dll.sh`; `.def` exports; `/W4 /WX` | libzmq 4.3.5 + libsodium **dynamic from vcpkg** (the DLL ships next to it in Sierra `Data/`); FlatBuffers headers vendored **25.1.24**; Eigen from `/usr/include/eigen3`; **no ctest/gtest** (tests are standalone `g++` programs); Rust spike branch `spike/rust-in-dll` (`98ec03f`), and Sierra **loaded** a Rust-linked DLL (umbrella §12.8 check 3 PASS); `pyproject.toml` = ruff/mypy config only; tracked `compile_commands.json` (absolute paths) |
| **schema** | `regenerate_schema.sh` (103 KB) derives `WORKSPACE_ROOT` = parent dir; C++ and Python only; refuses unless `flatc --version` = vendored headers | `mts_schema.fbs` (`MTS.Schema`, root `MTS_Envelope`, id `LBRN`), `backtest_schema.fbs` (`MTS.Backtest`, `BTST`); `schema_change_control_gate.py` greps the regen script for **exact strings**, including `mamba run -n mts python`, and hash-baselines it; contract header generator `scripts/generate_contract_header.py` |
| **lbrnet** | env **`mts`** (py 3.13, MKL, tf-nightly, torch, numba 0.67, numpy 2.5.3, pyzmq 27.1/zeromq 4.3.5); 3-file lock (`environment-linux-64.lock`, `requirements-mts.lock.txt`, `environment.lock.sha256`) via `scripts/refresh_environment_lock.sh`; CI from the explicit lock | HMM = `models/student_t_hmm.py` (numba) + `_gpu.py` (torch) + `regime_engine.py` + 5.8k-line trainer; **35 GB untracked data** (`data/scid` 21 GB, `data/raw` 12 GB, `data/training` 3.2 GB); `lbrnet/generated` 46 MB incl. 12 stale `MTS.bak_*` dirs; 86 uncommitted files; 19 `sys.path` hacks |
| **GUI** (MTS) | **shares `mts`** (editable lbrnet by absolute path); `requirements.txt` has pyzmq/ib_async; `pyproject.toml` = lint config only; unittest, no CI | Imports `lbrnet.generated.MTS.Schema.*` and `lbrnet.core.rc_enums`; ZMQ inventory in umbrella §8; a 10 MB `.keras` tracked |
| **Atratus** (reference only) | root `Cargo.toml` workspace (resolver 2), pyo3 0.22 abi3 cdylib, `sensor_core` pure crate, `atratus_sensor` staticlib + cbindgen, vendored libzmq via cc-rs, `[patch.crates-io]` vendored ibapi; conda-lock; CI Python-only | See §10 for what to copy and what not |

**⚠️ Security, before anything moves:** a hard-coded `GOOGLE_API_KEY` is committed in **`GUI/config.py:21` (MTS)**
and **`cpp/config.py:22` (MindfulTrader)**, including their git history. §9.4 has the fix.

---

## 2. Principles (the rules every file below follows)

### 2.1 Pure core, thin edges

- **The pure crates `mts_schema`, `mts_observation_vector` and `mts_hmm` carry no `pyo3`, `zmq` or Sierra code.** Only
  `std` and math crates. They build and test natively on Linux in seconds, and every golden test lives
  there.
- **The I/O and language edges are separate crates:**
  - `mts_transport` (zmq);
  - `mts_ffi` (C-ABI for the DLL);
  - `mts_py` (PyO3).
- Atratus proved this split: `ladder_core`/`sensor_core` are pure, while `atratus_rust` holds pyo3,
  ibapi and polars.

### 2.2 ONE staticlib for the DLL, ONE extension for Python (a deliberate deviation from umbrella §8/§12.1's `*_ffi/`, `*_py/`)

- **Each Rust `staticlib` bundles its own copy of `std` and `core`.** Linking two of them into one
  `MindfulTrader.dll` gives duplicate-symbol link errors, a known Rust limitation; the alternative is
  `/FORCE:MULTIPLE`, which is not acceptable here.
- So there is **one** `mts_ffi` staticlib, with Cargo features `hmm`, `observation_vector` and `transport`. It
  re-exports each subsystem's `extern "C"` functions from modules (`mts_ffi::hmm`, `mts_ffi::observation_vector`, …).
  Staged rollout = turning features on, not adding libraries.
- **Python likewise gets one `mindful_core` extension** with submodules (`mindful_core.hmm`,
  `.transport`, `.observation_vector`):
  - one build;
  - one install per env;
  - shared Rust types across submodules.

Record this deviation in the umbrella spec's decision register when W2 starts.

### 2.3 One version of everything that crosses a language boundary

**One FlatBuffers version (P2):** `flatc` = the vendored C++ headers = the Rust `flatbuffers` crate =
the Python `flatbuffers` runtime. Today they are four different things:
- the C++ headers are 25.1.24;
- the `mts` env's `flatc` is 24.3.25;
- the Python runtime is pinned to 25.9.23 (pip) / 24.3.25 (conda);
- Atratus pins the Rust crate to 24.3.25 against `flatc` 25.12.19.

Also one libzmq **per process image** (§6.3), one Rust toolchain (§3.1), and one numeric policy (§6.4).

### 2.4 Generated code is committed, and CI proves it is fresh

Generated FlatBuffers code (all three languages), the cbindgen header and the contract constants are
**committed**, so a plain checkout builds and diffs are reviewable. CI re-runs every generator and
fails on `git diff --exit-code` (§8). This replaces the cross-repo "three commits in three repos"
machinery (umbrella §3a).

### 2.5 Paths are derived, never absolute

Every script resolves its project root from its own location: `$(dirname "$0")` in bash,
`Path(__file__)` in Python, `${CMAKE_SOURCE_DIR}` in CMake. This is pre-work P1. **No new file may
contain `/home/rcruz/devel/VSCode`.** A pre-commit hook enforces it (§9.3).

### 2.6 Each Python project keeps its own env and lock (P6)

- **lbrnet keeps `mts`**, which its CLAUDE.md already declares lbrnet's own.
- **The GUI gets its own env.** The name is decided at P6; `mts-gui` is the proposal.
- The Rust extension is **installed into each**; it is never shared by putting one env's site-packages
  on the other's path.

---

## 3. The Rust side — files and contents

### 3.1 `rust-toolchain.toml` (repo ROOT, not `rust/`)

```toml
[toolchain]
channel = "1.98.1"                         # the toolchain on this machine 2026-10-08; bump deliberately
components = ["rustfmt", "clippy"]
targets = ["x86_64-pc-windows-msvc"]       # the Sierra DLL
profile = "minimal"
```

**Why the root:** rustup looks for this file in the **current directory and its parents**, not next to
the manifest. `cpp/build_dll.sh` runs `cargo --manifest-path ../rust/...` from `cpp/`. With the file
in `rust/`, that build would silently use whatever toolchain is the default. Atratus has no pin at
all (gap).

### 3.2 `rust/.cargo/config.toml`

```toml
[build]
target-dir = "target"                      # rust/target — one cache for every crate

[profile.release]
# the [profile] tables live in rust/Cargo.toml (§3.3); this file is only for per-machine/build env
```

- **Cargo also discovers `.cargo/config.toml` from the cwd upward.** So every script `cd`s into `rust/`
  before calling `cargo` (§7.1); `--manifest-path` alone is not enough.
- **Keep the Windows `CC_*`/`CFLAGS_*` cross variables out of this file.** `[env]` cannot expand
  `$HOME`, and the xwin sysroot path is per-machine. They live in `scripts/rust_windows_env.sh` (§7.1),
  sourced by the build scripts, exactly as `Atratus/cpp/build_dll.sh` passes them today.

### 3.3 `rust/Cargo.toml` (the workspace)

```toml
[workspace]
resolver = "3"
members = ["schema", "observation_vector", "hmm", "transport", "ffi", "py", "tools"]
default-members = ["schema", "observation_vector", "hmm", "transport", "tools"]   # `cargo test` never builds the cdylib/staticlib

[workspace.package]
edition = "2024"
rust-version = "1.98"
version = "0.1.0"
license = "LicenseRef-Proprietary"
publish = false

# Every third-party version lives HERE ONCE; crates say `foo.workspace = true`.
# (Atratus repeats versions per crate — ndarray/flatbuffers/zmq drift risk. Don't.)
[workspace.dependencies]
mts_schema    = { path = "schema" }
mts_observation_vector = { path = "observation_vector" }
mts_hmm       = { path = "hmm" }
mts_transport = { path = "transport" }
flatbuffers  = "=FB_VERSION"               # P2: exactly the flatc / C++ header / Python version
ndarray      = { version = "0.16", default-features = false, features = ["std"] }
special      = "0.10"
thiserror    = "2"
serde        = { version = "1", features = ["derive"] }
serde_json   = "1"
sha2         = "0.10"
zmq          = "0.10"                      # §6.3: link mode decided per stage
pyo3         = { version = "PYO3_VERSION", features = ["abi3-py313"] }   # current at W2; Atratus is stuck on 0.22
numpy        = "PYO3_MATCHING"             # rust-numpy's version tracks pyo3's
clap         = { version = "4", features = ["derive"] }
approx       = "0.5"
cbindgen     = "0.29"

[workspace.lints.rust]
unsafe_op_in_unsafe_fn = "deny"
missing_debug_implementations = "warn"

[workspace.lints.clippy]
all = { level = "deny", priority = -1 }
float_cmp = "deny"                         # parity code compares with stated tolerances (§6.4)
unwrap_used = "deny"                       # hot path / FFI: no panics by accident (tests may `allow`)

[profile.release]
opt-level = 3
lto = "thin"
codegen-units = 1                          # reproducible numerics and binary
debug = "line-tables-only"                 # symbolized crash reports from inside Sierra
panic = "unwind"                           # REQUIRED: catch_unwind at the C-ABI needs unwinding (§6.1)

[profile.dev.package."*"]
opt-level = 2                              # golden tests over millions of ticks stay fast in debug
```

**Notes:**
- **Edition 2024** makes `extern` blocks `unsafe extern` and `#[no_mangle]` → `#[unsafe(no_mangle)]`.
  The skeletons below use that spelling.
- **Never `panic = "abort"` for the staticlib.** It makes `catch_unwind` useless, so a Rust bug would
  kill Sierra.
- **No `-C target-cpu=native`** in any committed config. The DLL runs on whatever CPU the Sierra
  machine has, and parity tests must not depend on the build host (§6.4).

### 3.4 The crates (each `Cargo.toml` is tiny)

```toml
# rust/hmm/Cargo.toml — pure
[package]
name = "mts_hmm"
version.workspace = true
edition.workspace = true
rust-version.workspace = true
publish.workspace = true

[dependencies]
mts_schema.workspace = true                 # dim indices from the schema, never literals (lifecycle §3)
ndarray.workspace = true
special.workspace = true
thiserror.workspace = true
serde.workspace = true
serde_json.workspace = true
sha2.workspace = true

[dev-dependencies]
approx.workspace = true

[lints]
workspace = true
```

```toml
# rust/ffi/Cargo.toml — the ONE staticlib linked into MindfulTrader.dll
[package]
name = "mts_ffi"
# ... workspace fields ...

[lib]
crate-type = ["staticlib", "rlib"]          # rlib so its tests run natively on Linux

[features]
default = []
hmm = ["dep:mts_hmm"]
observation_vector = ["dep:mts_observation_vector"]
transport = ["dep:mts_transport"]

[dependencies]
mts_hmm = { workspace = true, optional = true }
mts_observation_vector = { workspace = true, optional = true }
mts_transport = { workspace = true, optional = true }

[build-dependencies]
cbindgen.workspace = true
```

```toml
# rust/py/Cargo.toml — the ONE Python extension
[package]
name = "mts_py"
# ... workspace fields ...

[lib]
name = "mindful_core"                       # the importable module name
crate-type = ["cdylib"]

[dependencies]
pyo3 = { workspace = true, features = ["extension-module"] }
numpy.workspace = true
mts_hmm.workspace = true
mts_observation_vector.workspace = true
mts_transport.workspace = true
```

### 3.5 `rust/py/pyproject.toml` (maturin)

```toml
[build-system]
requires = ["maturin>=1.7,<2"]
build-backend = "maturin"

[project]
name = "mindful-core"
requires-python = ">=3.13"
dynamic = ["version"]

[tool.maturin]
module-name = "mindful_core"
features = ["pyo3/extension-module"]
python-source = "python"                    # rust/py/python/mindful_core/__init__.py + .pyi stubs (typed for mypy)
```

- Ship a `mindful_core.pyi` stub with each submodule's signatures, so `mypy` in lbrnet and the GUI
  type-checks the Rust API. Atratus has no stubs; mypy sees `Any`.
- **abi3-py313:** one wheel serves both envs, which are both Python 3.13.

### 3.6 The FFI crate's shape (`rust/ffi/src/lib.rs`) — the rules from W0s, in code

```rust
//! C-ABI for MindfulTrader.dll. Rules (umbrella §12.8): every entry point is catch_unwind-guarded and
//! returns a status; state is created by mts_init and destroyed by mts_shutdown, which the study calls
//! on sc.LastCallToFunction — never in a static destructor or DLL_PROCESS_DETACH; no thread outlives
//! mts_shutdown; no allocation inside mts_*_step (buffers are sized at init).

use std::panic::{catch_unwind, AssertUnwindSafe};

#[repr(C)]
pub enum MtStatus { Ok = 0, BadArg = 1, NotInitialized = 2, Panicked = 3, Error = 4 }

fn guard(f: impl FnOnce() -> MtStatus) -> MtStatus {
    catch_unwind(AssertUnwindSafe(f)).unwrap_or(MtStatus::Panicked)
}

#[unsafe(no_mangle)]
pub extern "C" fn mts_abi_version() -> u32 { 1 }   // the study refuses to run on a mismatch

#[cfg(feature = "hmm")]
pub mod hmm;          // mts_hmm_init / mts_hmm_step / mts_hmm_reset / mts_hmm_shutdown
#[cfg(feature = "observation_vector")]
pub mod observation_vector;
#[cfg(feature = "transport")]
pub mod transport;    // submit(request) -> ticket / poll(ticket) — never blocks Sierra's thread (umbrella §8)
```

### 3.7 cbindgen (`rust/ffi/cbindgen.toml` + `build.rs`)

```toml
language = "C++"
namespace = "mts"
header = "// mts_core.h — generated by cbindgen from rust/ffi. DO NOT EDIT. Regenerate: scripts/build_rust.sh"
include_guard = "MTS_CORE_H"
line_endings = "LF"
[parse]
parse_deps = false
[export]
prefix = ""
```

- **`build.rs` writes `cpp/include/generated/mts_core.h`**, as Atratus's `atratus_sensor/build.rs` does
  for its header.
- **The header must compile cleanly under the DLL's `/W4 /WX`.** Add a 3-line
  `cpp/tests/compile_mt_core_h.cpp` that only includes it, so a warning fails the build early.
- **The header is committed.** CI re-runs the build and checks there is no diff (§2.4).

### 3.8 `rust/tools` (bins)

`hmm_tool` and `obs_tool`, with subcommands `train`, `posteriors`, `replay`, `verify-golden` and `export`
(lifecycle §3, umbrella §10). Native Linux binaries run from the repo root, so data paths are relative
to the root (`lbrnet/data/...`) and never absolute. Atratus's `atratus_tool` is the model: `clap` derive,
one binary, many subcommands.

---

## 4. The C++ side — CMake and presets

### 4.1 `cpp/CMakeLists.txt` — import the Rust lib (add, don't restructure)

```cmake
option(MTS_WITH_RUST "Link rust/ffi (mts_ffi.lib) into the DLL" OFF)   # ON once W2's gate passes

if(MTS_WITH_RUST)
  set(MTS_FFI_LIB "${CMAKE_SOURCE_DIR}/../rust/target/x86_64-pc-windows-msvc/release/mts_ffi.lib")
  if(NOT EXISTS "${MTS_FFI_LIB}")
    message(FATAL_ERROR "Missing ${MTS_FFI_LIB} — run cpp/build_dll.sh (it builds rust/ffi first).")
  endif()
  add_library(mts_ffi STATIC IMPORTED GLOBAL)
  set_target_properties(mts_ffi PROPERTIES
    IMPORTED_LOCATION "${MTS_FFI_LIB}"
    # `cargo rustc -p mts_ffi --target x86_64-pc-windows-msvc --release -- --print native-static-libs`
    # — re-run whenever a Rust dependency changes; Atratus's list was:
    INTERFACE_LINK_LIBRARIES "kernel32;ntdll;userenv;ws2_32;dbghelp;bcrypt;iphlpapi;legacy_stdio_definitions")
  target_link_libraries(MindfulTrader PRIVATE mts_ffi)
  target_compile_definitions(MindfulTrader PRIVATE MTS_WITH_RUST=1)
  # Rust links the dynamic CRT (/MD); the toolchain forces /DEFAULTLIB:libcmt.lib. Proven twice:
  # Atratus/cpp/CMakeLists.txt and the 2026-10-07 Sierra probe (umbrella §12.8 check 1).
  target_link_options(MindfulTrader PRIVATE "/NODEFAULTLIB:libcmt.lib")
endif()
```

- **Corrosion (CMake↔Cargo integration) was considered and is not used.** The cross build needs per-machine
  `CC_x86_64_pc_windows_msvc`/`CFLAGS` variables for cc-rs (vendored C deps), which the shell script
  already handles. One more build layer would hide exactly the CRT/sysroot detail that has to stay visible.
- **`cpp/build_dll.sh` runs cargo before CMake every time.** Cargo is incremental, so this is a no-op when
  nothing changed, and the DLL can never link a stale `mts_ffi.lib`.

### 4.2 `cpp/CMakePresets.json` — add a native test preset (today: no ctest at all)

```json
{
  "version": 6,
  "configurePresets": [
    { "name": "wsl-clang-cl-release", "displayName": "Sierra DLL (clang-cl + xwin)",
      "generator": "Ninja", "binaryDir": "${sourceDir}/build-windows",
      "toolchainFile": "${sourceDir}/toolchain-clang-cl.cmake",
      "cacheVariables": { "CMAKE_BUILD_TYPE": "Release", "CMAKE_EXPORT_COMPILE_COMMANDS": "ON",
                          "MTS_WITH_RUST": "OFF" } },
    { "name": "wsl-clang-cl-release-rust", "inherits": "wsl-clang-cl-release",
      "cacheVariables": { "MTS_WITH_RUST": "ON" } },
    { "name": "linux-tests", "displayName": "Native C++ tests (ctest)",
      "generator": "Ninja", "binaryDir": "${sourceDir}/build-linux-tests",
      "cacheVariables": { "CMAKE_BUILD_TYPE": "RelWithDebInfo", "CMAKE_CXX_COMPILER": "clang++",
                          "MTS_BUILD_TESTS": "ON" } }
  ],
  "buildPresets": [
    { "name": "dll", "configurePreset": "wsl-clang-cl-release" },
    { "name": "dll-rust", "configurePreset": "wsl-clang-cl-release-rust" },
    { "name": "tests", "configurePreset": "linux-tests" }
  ],
  "testPresets": [
    { "name": "tests", "configurePreset": "linux-tests", "output": { "outputOnFailure": true } }
  ]
}
```

- **`MTS_BUILD_TESTS`** adds one `add_executable` + `add_test` per `tests/cpp/test_*.cpp`. Today those are
  bare `g++` commands in file headers, so this turns 41 hand-run programs into `ctest --preset tests`
  for CI.
- **Keep `compile_commands.json` untracked** (`.gitignore`); the tracked one holds absolute paths. Point
  `.clangd` at `build-windows/`.

### 4.3 `-ffast-math`: settle it here (umbrella §10, §12.5-5)

- **What the CMake says:** `-O3 -march=native -ffast-math -mavx2` sits under `if(NOT MSVC)`.
- **Verify, don't assume:**
  - check `build-windows/compile_commands.json` for `-ffast-math`/`/fp:fast`;
  - CMake normally treats clang-cl as `MSVC`, in which case the DLL never got those flags and the
    non-MSVC branch only matters to native/tool builds.
- **Policy proposal for the operator:** no fast-math anywhere a value is compared against a golden —
  the DLL dims, the Rust crates, and the native tests. Rust has no fast-math by default; keep it that way.

---

## 5. Schema — one generator, three languages (P2, P3)

- **Same script, one more target:** `schema/regenerate_schema.sh` gains `flatc --rust` (+
  `--gen-object-api` only if Rust needs owned types) writing to `rust/schema/src/generated/`. The C++
  and Python targets are unchanged.
- **Rust contract constants:** extend `schema/scripts/generate_contract_header.py` to also emit
  `rust/schema/src/contract.rs` (dim indices, `OBSERVATION_FIELDS`, enum IDs), so `mts_observation_vector`/`mts_hmm`
  never hard-code a dim index.
- **The script's paths become monorepo-relative:** `ROOT="$(cd "$(dirname "$0")/.." && pwd)"`;
  targets `$ROOT/cpp/include/generated`, `$ROOT/lbrnet/lbrnet/generated`, `$ROOT/rust/schema/src/generated`.
- **The change-control gate (`schema_change_control_gate.py`) greps for exact strings**, including
  `mamba run -n mts python`.
  - Update its expected strings and its `.schema_change_control_baseline.json` hash **in the same
    commit** as the script edits, using its `--ack-no-regen-change` procedure.
  - Otherwise the gate goes red for a reason unrelated to the schema.
- **Stop the `*.bak_<timestamp>` backups.** In a single repo, git is the backup; delete the 12 stale
  `MTS.bak_*` dirs in `lbrnet/generated` (46 MB).
- **`flatc` comes from the pinned env:** add `flatbuffers=FB_VERSION` (conda-forge ships `flatc`) to
  the env that runs the regen. The script already refuses a mismatched `flatc`; keep that check.

### 5.1 From Atratus's `schema/regenerate_schema.sh`: copy this, not that (2026-10-09)

Atratus already has a working `flatc --rust` pipeline (`../Atratus/schema/regenerate_schema.sh`).
Surveyed read-only; concrete patterns worth reusing when P3 adds the Rust target to **our own**
`schema/regenerate_schema.sh` (not a replacement for it):

**Copy:**
- **Atomic generation via a scratch directory.** Generate into a throwaway `.work_gen/` first, copy
  into place only on success, then clean up — a failed/partial `flatc` run can never leave a
  half-written, broken target:
  ```bash
  rm -rf "$WORK_DIR"; mkdir -p "$WORK_DIR"
  flatc --rust -o "$WORK_DIR" "$SCHEMA_FILE"
  cp "$WORK_DIR"/*.rs "$RUST_TARGET/"
  rm -rf "$WORK_DIR"
  ```
- **A lint-suppressing wrapper module** around the generated code:
  ```rust
  #[allow(unused_imports)]
  #[allow(dead_code)]
  #[allow(clippy::all)]
  pub mod mts_schema_generated;
  ```
  Directly necessary for us, not optional: `rust/Cargo.toml`'s `[workspace.lints.clippy] all =
  "deny"` (§3.3) would otherwise fail the build the moment P3 lands — `flatc`-generated code is
  noise we don't control and can't fix.
- **A lightweight post-generation verification** (does the expected `*_generated.rs` file exist?)
  — fold into our own script's existing freshness checks (§2.4), don't invent a second mechanism.

**Do NOT copy:**
- **Dumping the generated Rust straight into one do-everything crate.** Atratus writes directly
  into `sensor_core/src/generated/`, so anything needing the schema must depend on all of
  `sensor_core`. We've already correctly diverged from this (§0, §3.3): a **dedicated `mts_schema`
  crate**, so `mts_hmm`/`mts_observation_vector`/`mts_transport` each depend on just the schema, not
  on each other transitively — the right call given we have more, more-separable consumers than
  Atratus does. Don't regress this under the influence of "but Atratus does it this way."
- **Dropping C++ bindings entirely.** Atratus's script generates only Python and Rust now ("since
  2026-10-05 the Sierra study is a tick pump linked to the Rust sensor... C++ -> Rust transition").
  Not applicable to us: `cpp/` is nowhere near being reduced to a thin pump, so P3 is additive (a
  third target alongside the existing C++/Python generation), never a replacement for either.


---

## 6. Cross-cutting runtime rules (bake into code reviews)

### 6.1 Rust inside Sierra's process (from W0s)

- Every `extern "C"` function is `catch_unwind`-guarded and returns a status (§3.6).
- `mts_init`/`mts_shutdown` are explicit, driven by `sc.LastCallToFunction`. Nothing relies on destructors
  at unload; no `static` holds a thread or socket past shutdown.
- **Unload/reload (umbrella §12.8 check 6) must pass** before any long-lived thread or socket ships in
  `mts_ffi`.
- **One FFI call per tick or bar-close, not per dim.**
- **No allocation in `*_step`.**

### 6.2 Python calls into Rust

Release the GIL around every blocking or long call (`py.allow_threads`, as Atratus's `IbSession` does).
Pass NumPy arrays zero-copy through `numpy` (rust-numpy) views; never Python lists in hot loops.

### 6.3 libzmq: one copy per process image (umbrella §12.5-4)

- **During the transition** (C++ still has its own sockets): `mts_transport` links the **same dynamic
  vcpkg libzmq** the DLL already uses, with no vendoring. Point zmq-sys's build script at the sysroot's
  `vcpkg/x64-windows` lib/include. **Read the exact env-var names from the zmq-sys version actually
  locked**; they have changed between releases.
- **After W9** (every socket in Rust): switch to the vendored build. libzmq is compiled into
  `mts_ffi.lib` as in Atratus; drop the vcpkg link and stop deploying `libzmq-mt-4_3_5.dll`.
- **Python processes** use `mindful_core`'s own libzmq. pyzmq leaves the envs only when no Python code
  imports `zmq` (P5 seam).

### 6.4 Numerics and parity

- **f64 inside the model, f32 on the wire**, as the Python HMM does (lifecycle §4).
- **Tolerances are stated in the test before the comparison runs.**
- **Goldens are language-neutral files** under `rust/<crate>/tests/goldens/`. Small ones are committed;
  big ones are referenced by sha256 + path under `lbrnet/data/` and skipped (not passed) when absent.

---

## 7. Scripts (repo root `scripts/`; all derive `ROOT` from their own path)

### 7.1 `scripts/rust_windows_env.sh` (sourced) and `scripts/build_rust.sh`

```bash
# rust_windows_env.sh — cc-rs cross variables for x86_64-pc-windows-msvc (from Atratus/cpp/build_dll.sh)
XWIN_SYSROOT="${XWIN_SYSROOT:-$HOME/.local/sysroots/x86_64-pc-windows-msvc}"
export CC_x86_64_pc_windows_msvc=clang-cl-22 CXX_x86_64_pc_windows_msvc=clang-cl-22
export AR_x86_64_pc_windows_msvc=/usr/lib/llvm-22/bin/llvm-lib
export CFLAGS_x86_64_pc_windows_msvc="--target=x86_64-pc-windows-msvc /winsysroot$XWIN_SYSROOT"
export CXXFLAGS_x86_64_pc_windows_msvc="$CFLAGS_x86_64_pc_windows_msvc"
```

```bash
# build_rust.sh [--features hmm,observation_vector,transport]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/rust_windows_env.sh"
cd "$ROOT/rust"                                   # .cargo/config.toml + rust-toolchain.toml discovery (§3.1-3.2)
cargo build -p mts_ffi --release --target x86_64-pc-windows-msvc --features "${FEATURES:-}"
git -C "$ROOT" diff --quiet -- cpp/include/generated/mts_core.h || echo "NOTE: mts_core.h changed — commit it"
```

- **Why `/winsysroot`:** the sysroot path has no spaces, unlike "Windows Kits", which cc-rs would split.
  That is why Atratus uses it.
- **`cpp/build_dll.sh`** calls `build_rust.sh` first when `MTS_WITH_RUST=ON`, then its existing
  CMake/Ninja steps, then checks the DLL's imports with `llvm-objdump-22 -p` (umbrella §12.8 check 2,
  now permanent).

### 7.2 `scripts/install_py_ext.sh` — never overwrite a loaded `.so`

```bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT/rust/py"
maturin build --release --out "$ROOT/rust/target/wheels"
WHEEL=$(ls -t "$ROOT"/rust/target/wheels/mindful_core-*.whl | head -1)
for ENV in mts mts-gui; do                        # P6 env names
  mamba run -n "$ENV" python -m pip install --no-deps --force-reinstall "$WHEEL"
done
```

**Atratus lesson, 2026-10-06:** `cp`-ing a rebuilt `.so` over the file a running GUI had mapped
**segfaulted the live GUI**. `pip install` unlinks and writes a new inode, so a running process keeps
its old mapping. Even so:
- install only with the GUI and backtest server stopped;
- never `cp` over the `.so`;
- avoid `maturin develop` into a shared env while another process uses it.

### 7.3 `scripts/check_all.sh` — the commit gate

The script runs these steps in order, and stops at the first failure:
1. `cargo fmt --check`
2. `cargo clippy --workspace --all-targets -D warnings`
3. `cargo test --workspace`
4. schema regen and `git diff --exit-code`
5. `ctest --preset tests`
6. lbrnet: `ruff` + `mypy` + `pytest -m "not slow"` (in its env)
7. GUI: `ruff` + `mypy` + `pytest` (in its env)

Wire the steps together with `&&`, never `;`. Run it as `scripts/check_all.sh && git commit ...`.
Atratus committed a red tree once because the checks were chained with `;`.

---

## 8. CI — `.github/workflows/ci.yml` (one workflow, path-filtered jobs)

```yaml
name: CI
on: { push: { branches: [master] }, pull_request: {} }
jobs:
  changes:                               # dorny/paths-filter → outputs: rust, cpp, schema, lbrnet, gui
  rust:      # if rust||schema: rust-toolchain.toml is honoured automatically
    steps: [checkout, "cargo fmt --check", "cargo clippy --workspace --all-targets -- -D warnings",
            "cargo test --workspace"]    # pure crates + mts_ffi as rlib; native Linux
  schema-fresh:  # if schema||rust||cpp||lbrnet: install pinned flatc; regenerate; git diff --exit-code
  cpp-tests:     # if cpp: cmake --preset linux-tests && ctest --preset tests
  lbrnet:        # if lbrnet||rust: micromamba from lbrnet's explicit lock; maturin build; pip install wheel; pytest -m "not slow and not integration"
  gui:           # if gui||rust: same, GUI env lock
  windows-cross: # LATER (optional): cargo-xwin build -p mts_ffi --target x86_64-pc-windows-msvc
```

- **What stays local:**
  - the full DLL link — it needs the xwin sysroot and the vcpkg tree;
  - anything needing Sierra or `lbrnet/data`.
- **`windows-cross` can be added later** with `cargo-xwin`, which fetches the MSVC CRT/SDK and needs
  the license accepted in CI. It catches CRT and symbol breaks before a manual build does.
- **Existing workflows fold in as jobs or steps:** lbrnet's `institutional-backtesting-gate.yml` and
  its lock-integrity check.

---

## 9. Repo-root files

### 9.1 `.gitignore` (root) — the 35 GB must never be staged

```gitignore
# build outputs
/rust/target/
/cpp/build-windows/  /cpp/build-linux-tests/  /cpp/compile_commands.json
*.dll  *.lib  *.pdb  *.so  *.whl
# data and models (lbrnet: 35 GB untracked today)
/lbrnet/data/  /lbrnet/models/**/*.pkl  /lbrnet/logs/
*.scid  *.context  *.alpha  *.btst  *.lbr  *.npy  *.parquet
# caches
__pycache__/  .mypy_cache/  .pytest_cache/  .ruff_cache/  node_modules/
# never again
*.bak_*  *.backup  *.stub_backup  errors.txt  SierraChart.log
.env  *.pem  *credentials*.json
```

**Merge Stage 2 gate:** `git status --porcelain | wc -l` in the merged tree, plus `du -sh` of what
`git add -A --dry-run` would stage, must be in **MB**, not GB.

### 9.2 `.gitattributes` and `.editorconfig`

```gitattributes
* text=auto eol=lf
*.fbs text diff
**/generated/** linguist-generated=true -diff     # quieter reviews; CI checks freshness instead
*.keras binary
*.png binary
*.wav binary
*.pdf binary
```

- **LF everywhere:** cbindgen and the `.def` exports are consumed on Windows but built in WSL.
- **LFS is not needed now:**
  - the largest tracked files are `tests/cpp/fixtures_dim*_raw.h` (~17 MB total) and the GUI's 10 MB
    `.keras`;
  - decide on LFS only if the tracked total passes ~200 MB.
- **`.editorconfig`:** 4 spaces for py/rs/cpp; the max line follows each project's ruff config.

### 9.3 `.pre-commit-config.yaml` (root)

**Hooks:**
- ruff (each project's own `pyproject.toml` config applies by directory);
- `cargo fmt`;
- the lock-hash checks:
  - lbrnet's three-file `sha256sum -c`;
  - the GUI env's lock;
- **`no-absolute-paths`**: rejects `/home/rcruz/devel/VSCode` in staged files outside `docs/` history;
- **gitleaks**: it would have caught the two API keys;
- **the four-mirror doc sync**, once its real source is found (umbrella Stage 0).

### 9.4 Secrets — fix before the merge, not after

1. **Rotate** the Google API key(s) in `MTS/config.py:21` and `MindfulTrader/config.py:22` now. Rotation
   is the real fix; history scrubs only stop further spread.
2. Move them to the environment or `keyring`, namespaced per project; Atratus keeps its secrets in
   `keyring`. Read them with `os.environ[...]` and fail loudly when missing.
3. **History, by repo:**
   - **MTS** is already rewritten by `git filter-repo --to-subdirectory-filter GUI` (umbrella §4). Add
     `--replace-text` in the same throwaway clone; it costs nothing.
   - **MindfulTrader is the base**, kept without a force-push. Scrubbing its history means a
     force-push of `felocruz/MindfulTrader`, which is the **operator's call**. With the key rotated,
     leaving the history is defensible.

### 9.5 `MindfulTrader.code-workspace` (multi-root; replaces `MTS GUI.code-workspace` and per-repo `.vscode/`)

```json
{
  "folders": [
    { "path": ".", "name": "root" },
    { "path": "rust", "name": "rust (Rust)" },
    { "path": "cpp", "name": "cpp (Sierra DLL)" },
    { "path": "lbrnet", "name": "lbrnet" },
    { "path": "GUI", "name": "GUI" },
    { "path": "schema", "name": "schema" }
  ],
  "settings": {
    "rust-analyzer.linkedProjects": ["rust/Cargo.toml"],
    "rust-analyzer.cargo.targetDir": "rust/target/ra",
    "files.exclude": { "**/target": true, "lbrnet/data": true },
    "files.watcherExclude": { "lbrnet/data/**": true, "rust/target/**": true }
  }
}
```

- **Per-folder interpreters:** set `python.defaultInterpreterPath` in `lbrnet/.vscode/settings.json` and
  `GUI/.vscode/settings.json` (envs `mts` / `mts-gui`).
- **`cpp/.vscode/settings.json`** points clangd at `build-windows/compile_commands.json`.
- **rust-analyzer gets its own target dir** so it doesn't fight `build_rust.sh` for the cargo lock.

---

## 10. From Atratus: copy this, not that

| Copy (proven) | Do better than Atratus |
|---|---|
| Pure core crates separate from pyo3/zmq/ibapi crates | **Pin the toolchain** (`rust-toolchain.toml` at the root); Atratus has none |
| xwin + clang-cl-22 + `/winsysroot` cc-rs variables; `/NODEFAULTLIB:libcmt.lib`; `--print native-static-libs` | **`[workspace.dependencies]`**: Atratus repeats versions per crate |
| cbindgen-generated C++ header from `build.rs` | **One FlatBuffers version**: Atratus's crate is 24.3.25 while its `flatc` is 25.12.19 |
| `catch_unwind` + explicit init/shutdown (Atratus has the `OnceLock` singleton, so it needs this rule *more*) | **Scripted wheel install** (§7.2), never a `.so` copy; Atratus's live GUI segfaulted on one |
| `[patch.crates-io]` with a vendored, commented crate when upstream needs a fix (`third_party/<crate>`, every change marked `VENDOR PATCH`) | **Rust in CI**: Atratus's CI is Python-only |
| Per-project env + lock; CI from the lock; pre-commit lock-hash check | **Small crates**: `atratus_rust` pulls polars + tract-onnx + plotters into one cdylib (slow builds). Add a heavy dependency only to the crate that needs it |
| Commit gate: checks `&&` commit | **Prove it in Sierra**: Atratus's Rust DLL was never loaded in Sierra. Here W0s already passed load (check 3); finish checks 5–9, above all **6 (unload/reload)**, before `mts_ffi` grows threads |
| Python-free hot path: Python only displays and forwards | `.pyi` stubs for the extension (mypy sees `Any` in Atratus) |

---

## 11. Order of operations (maps onto the umbrella's sequencing; gates per step)

**Superseded ordering, 2026-10-08**: steps 5-6 (finish W0s, then the merge) and step 7 (W2
bootstrap) are no longer sequential as written below — see
`docs/superpowers/plans/2026-10-08-monorepo-rust-adoption-roadmap.md` §1: the merge (step 6) is
decoupled from Rust work and deferred to its own schedule; step 7's `rust/` scaffold and `mts_ffi`
skeleton were started and partly completed **before** step 6, not after. The per-step gates below
still apply to each step individually; only the relative order across steps 5-7 has changed.

| # | Step | Umbrella ref | Gate |
|---|---|---|---|
| 1 | Rotate the API keys; add gitleaks to each repo's pre-commit | §9.4 here | gitleaks clean on HEAD |
| 2 | P2: one FlatBuffers version; install the matching `flatc` in the regen env; regenerate | P2 | regen diff = version asserts only; all three projects' checks green |
| 3 | P1: root-relative paths (+ the `no-absolute-paths` hook per repo) | P1 | `git grep /home/rcruz/devel/VSCode` → docs history only |
| 4 | P6: split envs + locks (`mts` = lbrnet; `mts-gui` = GUI); GUI depends on lbrnet by a relative editable path (`-e ../lbrnet`) until the merge | P6 | each project's tests green in its own env |
| 5 | W0s checks 5–9 (above all **6**) with the spike's safe method (worktree, separate DLL name, separate log) | §12.8 | **Resolved without an empirical run** — 2026-10-08, roadmap plan §2: check 6 reasoned through mechanism-by-mechanism, no applicable failure mode found. Checks 1,3,4,5,7,8 PASSED with real evidence (two independent spikes); 2,9 not reverified/not attempted, low priority. |
| 6 | The merge, Stages 0–6, with §9.1's `.gitignore` in place **before** the first `git add` in the scratch repo | §4–5 | umbrella gates + the MB-not-GB check; **deferred to its own schedule, 2026-10-08** — no longer a prerequisite for step 7 |
| 7 | **W2 bootstrap:** root files (§9), `rust-toolchain.toml`, `rust/` workspace with **empty** crates, `mts_ffi` exporting only `mts_abi_version`, `MTS_WITH_RUST` preset, `build_rust.sh`, `install_py_ext.sh` (`mindful_core.version()`), `check_all.sh`, CI | W2 | **Partly done, 2026-10-08** (commit `63bcc63`): `rust-toolchain.toml`, `rust/` workspace, `mts_ffi` skeleton, `MTS_WITH_RUST` preset, `build_rust.sh` all exist and are proven (DLL builds with `-rust` preset; imports confirmed unchanged via objdump). **Not yet done**: Sierra load-and-log round-trip for this specific skeleton (deferred — W0s spikes already give that confidence independently), `install_py_ext.sh`, `check_all.sh`, CI. |
| 8 | P3: `flatc --rust` + `contract.rs` into `rust/schema`; `mts_schema` compiles; freshness job | P3 | `schema-fresh` CI job green |
| 9 | Then the umbrella's W3 (`mts_hmm` inference vs P9 goldens) ∥ W5 (`mts_observation_vector` vs P8 goldens) → W6 → W7 → W9 | §12.3 | as specified there |

**Doc sync, when W2 lands:** "Rust is a build prerequisite for `cpp/`" goes into the four mirror docs
(umbrella §6-8); the deviation in §2.2 goes into the umbrella's decision register (§12.4); and a
`PRODUCTION_TRIAGE.md` row is added for the program (umbrella §12.5-7).

---

## 12. Open questions for the operator (not decided by this guide)

1. ~~**One `mts_ffi` and one `mindful_core`** instead of the umbrella's per-subsystem `*_ffi`/`*_py`
   (§2.2)?~~ **Ratified by implementation, 2026-10-08**: built and proven as one `mts_ffi` staticlib
   (commit `63bcc63`). Still open for `mindful_core`/`mts_py` — not built yet.
2. **GUI env name** (`mts-gui` proposed) and whether lbrnet's env keeps the name `mts` (it owns it today).
3. **Numeric policy** (§4.3): no fast-math wherever goldens compare. Recommended.
4. **MindfulTrader history scrub** (force-push) vs rotation only (§9.4).
5. **`windows-cross` CI job** now (cargo-xwin) or later.
