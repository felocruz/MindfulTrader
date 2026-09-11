## Cross-Compile Sysroot Migration (`/mnt/c` → native `~/.local/sysroots/`)

**Status: Phase A DONE and validated on this machine, 2026-09-10.** Opened after a Puget-machine
`libzmq` build failure and a broader push to stop depending on `/mnt/c` for the Windows cross-compile
toolchain. Phase B (CRT/SDK splat) is still brainstorm / not yet implemented.

**Phase B is now MANDATORY, not optional (confirmed 2026-09-10): Puget will not have Visual Studio
installed at all.** There is no `/mnt/c/Program Files/Microsoft Visual Studio/...` to fall back to
on that machine — without an `xwin`-splatted CRT/SDK, `clang-cl` has no MSVC headers/libs to build
against, period. This resolves open question #2 below: yes, `xwin` fully retires the Windows-side
VS Installer requirement (`docs/NEW_MACHINE_WSL_SETUP.md` step 9) for Puget — that step must be
skipped there, not just made redundant.

## Problem

This repo cross-compiles `MindfulTrader.dll` from WSL using `clang-cl` targeting the MSVC ABI
(`toolchain-clang-cl.cmake`'s `TRIPLE = x86_64-pc-windows-msvc`), and links against vcpkg-built
`x64-windows` libraries. All of it currently resolves through `/mnt/c/...`:

- `toolchain-clang-cl.cmake`: `MSVC_ROOT_DIR` / `WINDOWS_SDK_ROOT` (defined near the top of the
  file, §0) under `/mnt/c/Program Files/Microsoft Visual Studio/...` and
  `/mnt/c/Program Files (x86)/Windows Kits/10`. Both variables are consumed twice further down in
  the same file: as compiler flags (`/vctoolsdir "${MSVC_ROOT_DIR}"`, `/winsdkdir
  "${WINDOWS_SDK_ROOT}"`, §5) and as linker flags (`/LIBPATH:"${MSVC_LIB_DIR}"`,
  `/LIBPATH:"${SDK_LIB_UCRT_DIR}"`, `/LIBPATH:"${SDK_LIB_UM_DIR}"` plus the `/DEFAULTLIB:...` list,
  §6). Phase B below only needs to repoint these two root variables — every downstream flag
  derives from them.
- `CMakeLists.txt`: vcpkg `include`/`lib`/`debug/lib` under
  `/mnt/c/Users/rcruz/vcpkg/installed/x64-windows/...`

`/mnt/c` is a 9p network-protocol mount — file I/O across it is well-documented as much slower than
native ext4, which is the likely root cause of the Puget `libzmq` build friction (this is a build
of *our own* code linking a prebuilt vcpkg `.lib`, not a from-scratch libzmq compile — so the
failure is almost certainly toolchain/path resolution across the mount, not libzmq's own source).

**Not MinGW** — confirmed no MinGW toolchain is in play (`grep` for `mingw` across the repo only
turns up an unrelated comment in a Sierra Chart header). This is 100% MSVC-ABI via `clang-cl`,
which is exactly the combination the `xwin` sysroot-splat convention (below) targets.

## Institutional standard: `xwin`-style sysroot splat

Elite/production shops cross-compiling Windows targets from Linux (popularized by
[Jake-Shadle/xwin](https://github.com/Jake-Shadle/xwin), adopted by `cargo-xwin` and various C++
Windows-cross setups) materialize the MSVC CRT + Windows SDK onto the native Linux filesystem once,
then point the toolchain at that — never at a live Windows-side install through a network mount.
`xwin splat` pulls the same NuGet manifest payloads the VS Installer uses (no VS install required)
and produces:

```
<splat-output>/
├── crt/
│   ├── include/                # MSVC STL + CRT headers
│   └── lib/x64/                # vcruntime.lib, msvcrt.lib, libcmt.lib, ...
└── sdk/
    ├── include/
    │   ├── ucrt/  um/  shared/  winrt/
    └── lib/x64/
        ├── ucrt/
        └── um/
```

## Proposed layout on this machine (staging, to be copied to Puget)

```
~/.local/sysroots/x86_64-pc-windows-msvc/     # <name> = the toolchain's own TRIPLE var
├── crt/include/, crt/lib/x64/                # from `xwin splat`
├── sdk/include/{ucrt,um,shared,winrt}/        # from `xwin splat`
├── sdk/lib/x64/{ucrt,um}/                     # from `xwin splat`
└── vcpkg/x64-windows/
    ├── include/                # zmq.h, zmq.hpp, zmq_addon.hpp, zmq_utils.h, sodium.h, sodium/
    ├── lib/                    # libzmq-mt-4_3_5.lib, libsodium.lib
    ├── debug/lib/               # libzmq-mt-gd-4_3_5.lib, libsodium.lib (debug)
    └── bin/, debug/bin/         # libzmq-mt-4_3_5.dll(+.pdb), libsodium.dll(+.pdb)
```

Native-Linux-side tooling (pip `--user` cmake, etc.) stays in bare `~/.local/{bin,lib,include}` —
unrelated to the Windows target, unchanged from the existing convention in
`docs/NEW_MACHINE_WSL_SETUP.md`.

### Why `vcpkg/x64-windows` is a **copy**, not a rebuild

The immediate reported problem (Puget `libzmq` build failure) is solved by copying the
already-built vcpkg artifacts from this machine — no native Linux vcpkg build needed. Confirmed
present on this machine at `/mnt/c/Users/rcruz/vcpkg/installed/x64-windows/`:

| File | Purpose |
|---|---|
| `lib/libzmq-mt-4_3_5.lib`, `debug/lib/libzmq-mt-gd-4_3_5.lib` | ZMQ static import libs (release/debug) |
| `bin/libzmq-mt-4_3_5.dll(+.pdb)` | ZMQ runtime DLL |
| `lib/libsodium.lib`, `debug/lib/libsodium.lib` | libsodium static import libs |
| `bin/libsodium.dll(+.pdb)`, `debug/bin/libsodium.dll(+.pdb)` | libsodium runtime DLL |
| `include/zmq.h`, `zmq.hpp`, `zmq_addon.hpp`, `zmq_utils.h` | ZMQ C + `cppzmq` C++ headers |
| `include/sodium.h`, `include/sodium/` | libsodium headers |

vcpkg package versions: `zeromq` 4.3.5, `libsodium` 1.0.20, `cppzmq` 4.11.0 (all `x64-windows`).
The vcpkg tree also has a large unrelated Boost/OpenSSL/etc. install that this project's
`CMakeLists.txt` does not actually link against (confirmed: only `libzmq*`/`libsodium*` appear in
`target_link_libraries`, and no `#include <boost/...>` exists anywhere in `src/`/`include/`).

**One more header-only dependency found during Phase A execution, not in the original inventory
above:** `nlohmann_json` 3.11.3 (`include/nlohmann/`) — used extensively across the codebase
(`FeatureScaler.h`, `RiskManager.cpp`, `HMMClient.cpp`, `ConfigManager.cpp`, and 30+ other sites).
Missing it produced a real build failure (`fatal error: 'nlohmann/json.hpp' file not found`) before
it was added to the sysroot's `include/` alongside zmq/sodium. Grepped for other common vcpkg
header-only packages (`rapidjson`, `spdlog`, `fmt`, `catch2`/`gtest`, `yaml-cpp`, etc.) — none in use.

### Phase A execution log (this machine, 2026-09-10)

1. Created `~/.local/sysroots/x86_64-pc-windows-msvc/vcpkg/x64-windows/{include,lib,debug/lib,bin,debug/bin}`.
2. Copied all files in the inventory table above, plus `include/nlohmann/` (45 headers) once the
   missing dependency surfaced — 91 files, 14M total.
3. Updated `CMakeLists.txt`: introduced one `VCPKG_SYSROOT` cache-free variable
   (`$ENV{HOME}/.local/sysroots/x86_64-pc-windows-msvc/vcpkg/x64-windows`) and replaced all 4
   `/mnt/c/Users/rcruz/vcpkg/installed/x64-windows/...` references (`include_directories`,
   `target_include_directories`, `link_directories`, `target_link_libraries`) with it.
4. `./build_dll.sh --no-clean` — succeeded end to end (compile + link) after step 2's fix;
   `build-windows/bin/MindfulTrader.dll` produced (1.8M). Verified `build-windows/build.ninja` has
   48 references to the new sysroot path and zero remaining to `/mnt/c/Users/rcruz/vcpkg` — clean
   cutover, not a stale/mixed config.
5. **Validated live**: deployed via `./deploy_mindfultrader.sh` and confirmed loading successfully
   inside Sierra Chart itself, not just a clean rebuild.
6. **Transfer to Puget, chosen method: GitHub Release asset** (both machines already have `gh`
   auth + clone access, no direct network link between the two boxes exists, and no physical
   media/USB needed). Executed:
   - `tar -czvf ~/mindfultrader-vcpkg-sysroot-x64-windows.tar.gz -C ~/.local/sysroots/x86_64-pc-windows-msvc vcpkg/`
     (3.6M compressed from 14M).
   - `gh release create sysroot-vcpkg-x64-windows-20260910 ~/mindfultrader-vcpkg-sysroot-x64-windows.tar.gz --repo felocruz/MindfulTrader --title "..." --notes "..." --target master`
   - Release: https://github.com/felocruz/MindfulTrader/releases/tag/sysroot-vcpkg-x64-windows-20260910

   **On Puget, to complete the transfer:**
   ```bash
   mkdir -p ~/.local/sysroots/x86_64-pc-windows-msvc
   cd ~/.local/sysroots/x86_64-pc-windows-msvc
   gh release download sysroot-vcpkg-x64-windows-20260910 -R felocruz/MindfulTrader
   tar -xzvf mindfultrader-vcpkg-sysroot-x64-windows.tar.gz
   rm mindfultrader-vcpkg-sysroot-x64-windows.tar.gz
   ```
   This reconstructs `~/.local/sysroots/x86_64-pc-windows-msvc/vcpkg/x64-windows/{include,lib,debug/lib,bin,debug/bin}`
   at the exact path `CMakeLists.txt`'s `VCPKG_SYSROOT` variable expects — `./build_dll.sh` should
   build clean on Puget with no further `CMakeLists.txt` changes needed. **Not yet confirmed on
   Puget** — once it builds there, delete the release (`gh release delete
   sysroot-vcpkg-x64-windows-20260910 --repo felocruz/MindfulTrader`), it's a one-time transfer
   artifact, not a real code release.

### Why CRT/SDK is a **splat**, not a copy

Unlike vcpkg's output, the MSVC CRT + Windows SDK aren't portable files to hand-copy licensing-wise
or practically (they're huge, versioned, and tied to the VS installer's servicing). `xwin splat` is
the correct mechanism on *each* machine — but it only needs to run once per machine and produces a
byte-identical result for a given MSVC/SDK version, so both machines end up consistent without
depending on `/mnt/c` at all going forward.

**Blocker on this machine:** `xwin`/`cargo`/`rustc` are not currently installed here (verified via
`which`). Needed before this phase can execute: either install Rust + `cargo install xwin`, or fetch
a prebuilt `xwin` release binary for `x86_64-unknown-linux-gnu` (no cargo required).

## Proposed phases

**Phase A — vcpkg artifact copy (solves the reported Puget failure, no new tooling needed) — DONE, see execution log above**
1. ~~On this machine: `mkdir -p ~/.local/sysroots/x86_64-pc-windows-msvc/vcpkg/x64-windows/{include,lib,debug/lib,bin,debug/bin}`~~
2. ~~Copy the files in the table above from `/mnt/c/Users/rcruz/vcpkg/installed/x64-windows/` into
   the matching subdirs.~~
3. ~~Update `CMakeLists.txt`'s three vcpkg reference points~~ — done via one `VCPKG_SYSROOT` variable.
4. ~~Rebuild on this machine (`./build_dll.sh`) to confirm zero regression before touching anything
   else.~~ — succeeded.
5. **Remaining:** copy the whole `~/.local/sysroots/x86_64-pc-windows-msvc/vcpkg/` subtree to Puget
   (tarball over LAN/USB/share — same spirit as `docs/NEW_MACHINE_WSL_SETUP.md` step 13's data-file
   transfer).

**Phase B — CRT/SDK splat (MANDATORY for Puget — no VS install exists there to fall back on)**
1. Install `xwin` (prebuilt binary preferred, avoids needing a Rust toolchain here).
2. Run `xwin splat` targeting MSVC `14.44.35207` / SDK `10.0.26100.0` (this machine's current
   `toolchain-clang-cl.cmake` values) into `~/.local/sysroots/x86_64-pc-windows-msvc/{crt,sdk}/`.
   If `xwin` can't pin those exact versions, splat whatever it resolves and update
   `toolchain-clang-cl.cmake`'s `MSVC_VERSION`/`SDK_VERSION` to match — on Puget there is no local
   VS install to "verify against" per `docs/NEW_MACHINE_WSL_SETUP.md` step 11 (that step is now
   obsolete for Puget, see status banner above), so whatever `xwin` resolves simply becomes the new
   pinned version, on both machines.
3. Update `toolchain-clang-cl.cmake`'s `MSVC_ROOT_DIR`/`WINDOWS_SDK_ROOT` to point at
   `crt/`/`sdk/` under the new sysroot instead of `/mnt/c/Program Files...`.
4. Rebuild, verify `build-windows/bin/MindfulTrader.dll` output is unchanged (same exports, same
   size order of magnitude) before considering this done.
5. Copy `{crt,sdk}/` to Puget alongside the Phase A `vcpkg/` subtree (GitHub Release asset, same
   mechanism as Phase A step 6 — likely a much larger tarball, may need multiple release assets or
   splitting), or re-run `xwin splat` natively on Puget instead (equally valid since the splat
   output is deterministic per version, and avoids transferring a potentially large file at all).

## Open questions

1. Exact `<name>` under `~/.local/sysroots/` — assumed `x86_64-pc-windows-msvc` (matches
   `toolchain-clang-cl.cmake`'s own `TRIPLE`), needs confirmation against whatever's already
   in progress on Puget.
2. Does Puget's VS/SDK install (if any survives this migration) still need to match
   `14.44.35207`/`10.0.26100.0`, or does moving to `xwin` retire the Windows-side VS Installer
   requirement (`docs/NEW_MACHINE_WSL_SETUP.md` step 9) entirely?
3. `xwin` install method on this machine — prebuilt release binary vs. installing a Rust toolchain
   just to `cargo install` one tool (leaning prebuilt binary, avoids an otherwise-unneeded Rust
   dependency in a C++ repo).
4. Should `docs/NEW_MACHINE_WSL_SETUP.md` be rewritten to replace steps 9-11 (Windows-side VS +
   vcpkg + toolchain-file version verification) once this lands, per the Doc Sync Contract?
