## Cross-Compile Sysroot Migration (`/mnt/c` → native `~/.local/sysroots/`)

**Status: Phases A and B both DONE and validated on the old (now-retired) Dell machine, 2026-09-10.**
**Puget-side status (2026-09-13, live): IN PROGRESS — see `docs/PUGET_SETUP_COORDINATION.md`
Entries 1-2 for the current, verified state, not this file's older narrative below.** Puget's
vcpkg artifacts transferred and confirmed working as-is; the CRT/SDK splat, however, landed in a
differently-shaped layout than the old machine's (`crt`/`sdk` flat dirs, `x86_64` not `x64` arch
subdirs) that the committed `toolchain-clang-cl.cmake` doesn't yet match. An empirically-validated
fix (repoint the toolchain file at the flat layout directly, no re-splat needed) is written up in
the coordination doc but **not yet applied or build-verified** — don't treat this file's Phase B
section below as Puget's actual state until that lands and this file is rewritten to match (Doc
Sync Contract).

Opened after a Puget-machine
`libzmq` build failure and a broader push to stop depending on `/mnt/c` for the Windows
cross-compile toolchain — made mandatory once it was confirmed Puget will have no Visual Studio
install at all. **Remaining work is entirely Puget-side**: transferring/regenerating the sysroot
there and validating its own build. See the `~/.local` inventory section near the end for what
else (beyond this sysroot) needs to exist on Puget for this repo's workflow.


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

### Phase B execution log (this machine, 2026-09-10)

1. **Installed `xwin` 0.10.0** — no sudo, no Rust/cargo needed. Fetched the prebuilt
   `xwin-0.10.0-x86_64-unknown-linux-musl.tar.gz` release asset directly via `gh release download
   0.10.0 --repo Jake-Shadle/xwin` (note: tag has no `v` prefix, `gh release download v0.10.0`
   404s), verified its `.sha256` (bare-hash format, not `sha256sum -c`-compatible — compared
   manually), extracted, copied the single static binary to `~/.local/bin/xwin` (already on
   `PATH` per `docs/NEW_MACHINE_WSL_SETUP.md`'s pip `--user` convention).
2. **`xwin --accept-license list`** confirmed the default "latest" manifest already resolves to
   CRT `14.44.17.14` (the VS 2022 17.14 release containing MSVC toolset `14.44.35207`) and SDK
   `10.0.26100` — matching this machine's pinned `toolchain-clang-cl.cmake` versions. Total
   download only ~83 MiB (not the ~1GB originally feared).
3. **First splat attempt used plain defaults** (`--preserve-ms-arch-notation` only, no
   `--use-winsysroot-style`) — produced a **flat** `crt/{include,lib/x64}` +
   `sdk/{include,lib}/{ucrt,um,shared,...}/x64` layout with **no SDK-version-numbered subdirectory**
   (i.e. `sdk/include/ucrt`, not `sdk/include/10.0.26100/ucrt`). This is **incompatible** with
   `toolchain-clang-cl.cmake`'s current `/winsdkdir` + `/winsdkversion:${SDK_VERSION}` flags, which
   require that nested version folder (mirroring a real VS/SDK install) — `xwin`'s own `--help`
   text for `--use-winsysroot-style` explicitly warns about this exact mismatch and recommends
   switching to clang-cl's single `/winsysroot` flag instead for consumption of its default splat.
4. **Second (correct) splat**, deleting the first attempt's `crt/`/`sdk/` first, then:
   ```bash
   xwin --accept-license --temp --crt-version 14.44.17.14 --sdk-version 10.0.26100 \
     splat --output ~/.local/sysroots/x86_64-pc-windows-msvc \
     --preserve-ms-arch-notation --disable-symlinks --use-winsysroot-style
   ```
   (version pins placed *before* `splat` — `--crt-version`/`--sdk-version`/`--temp` are top-level
   `xwin` options, not `splat` subcommand options; passing them after `splat` errors with
   `unexpected argument`.) This produced the **real VS-mirroring nested layout**:
   ```
   ~/.local/sysroots/x86_64-pc-windows-msvc/
   ├── VC/Tools/MSVC/14.44.17.14/{include, lib/x64}          # 110M
   ├── Windows Kits/10/{Include,Lib}/10.0.26100/{ucrt,um,shared,winrt,cppwinrt}/  # 520M
   └── vcpkg/x64-windows/...                                   # 15M, from Phase A, untouched
   ```
5. **Real version-naming mismatch, discovered and fixed:** `toolchain-clang-cl.cmake`'s
   `MSVC_VERSION` (was `"14.44.35207"`, the real MSVC *toolset* version) and `SDK_VERSION` (was
   `"10.0.26100.0"`, the real SDK's 4-part on-disk folder name incl. trailing `.0`) did **not**
   match the directory names `xwin` actually created on disk (`VC/Tools/MSVC/14.44.17.14` — the VS
   *product* version, not the toolset version; `Windows Kits/10/Include/10.0.26100` — 3-part, no
   trailing `.0`). Both refer to the same underlying binaries/headers, just named differently by
   `xwin`'s manifest vs. a real VS installer. **Fixed**: introduced `XWIN_SYSROOT` as the single
   root variable, changed `MSVC_VERSION`/`SDK_VERSION`'s values to the actual on-disk names
   (`14.44.17.14` / `10.0.26100`), and rebuilt `MSVC_ROOT_DIR`/`WINDOWS_SDK_ROOT` from
   `${XWIN_SYSROOT}` — every downstream flag in the file derives from those two, so no other edits
   were needed. `"Windows Kits"`'s space is already handled correctly (existing quoted-path
   pattern in the file).
6. **Second real bug, found via actual rebuild attempt:** first corrected splat used
   `--disable-symlinks` (copying `xwin --help`'s own suggested flags for `--use-winsysroot-style`
   verbatim) — build failed with `fatal error: 'windows.h' file not found`
   (`sierra_chart_dependencies/scstructures.h`). Root cause: that flag combination is recommended
   by `xwin` **for running clang-cl on Windows itself** (case-insensitive host filesystem) — we're
   cross-compiling from **Linux** (case-sensitive ext4), which is exactly the scenario the default
   casing-fix symlinks (`windows.h` → `Windows.h`) exist to solve. Re-splatted without
   `--disable-symlinks` (keeping `--preserve-ms-arch-notation --use-winsysroot-style` only) —
   confirmed the symlink now exists, rebuilt clean.
7. **Fully validated**: `rm -rf build-windows && ./build_dll.sh` succeeded end to end (configure +
   compile + link), producing `MindfulTrader.dll` at **exactly the same size** (1,798,656 bytes) as
   the Phase A build — byte-level consistency. `build-windows/build.ninja` has **zero** `/mnt/c`
   references (down from 48) and 95 references to the new sysroot. Confirmed still a valid
   `PE32+ executable (DLL) (GUI) x86-64, for MS Windows` via `file`.

**Blocker resolved:** `xwin`/`cargo`/`rustc` are no longer a blocker — `xwin` is installed at
`~/.local/bin/xwin` per step 1 above. **Phase B is DONE on this machine** — only the Puget-side
transfer/re-splat and its own validation remain.

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

**Phase B — CRT/SDK splat (MANDATORY for Puget — no VS install exists there to fall back on) — DONE, see execution log above**
1. ~~Install `xwin`~~ — done (`~/.local/bin/xwin`, no sudo).
2. ~~Run `xwin splat`~~ — done. Correct invocation (two wrong attempts first, see execution log
   steps 3+6): `xwin --accept-license --temp --crt-version 14.44.17.14 --sdk-version 10.0.26100
   splat --output <dir> --preserve-ms-arch-notation --use-winsysroot-style` (no
   `--disable-symlinks` — needed on Linux hosts for header-casing symlinks), producing
   `VC/Tools/MSVC/14.44.17.14/` + `Windows Kits/10/{Include,Lib}/10.0.26100/`.
3. ~~Update `toolchain-clang-cl.cmake`~~ — done: added `XWIN_SYSROOT`, updated `MSVC_VERSION`/
   `SDK_VERSION` to the real on-disk names, `MSVC_ROOT_DIR`/`WINDOWS_SDK_ROOT` now derive from
   `XWIN_SYSROOT`.
4. ~~Rebuild, verify output unchanged~~ — done: identical byte size (1,798,656) to the Phase A
   build, zero `/mnt/c` references left in `build.ninja`, valid PE32+ DLL.
5. **Remaining:** copy `{VC,Windows Kits}/` to Puget alongside the Phase A `vcpkg/` subtree (GitHub
   Release asset, same mechanism as Phase A step 6 — this is ~630M uncompressed, likely needs
   compression and may still exceed comfortable single-asset size, worth checking compressed size
   before choosing), or re-run the exact pinned `xwin splat` command from step 2 natively on Puget
   instead (equally valid since the splat output is deterministic per version, and avoids
   transferring a large file at all — probably the better default choice given the size).

**Validated live in Sierra Chart, this machine, 2026-09-10** — DLL built from the Phase B
toolchain deployed via `./deploy_mindfultrader.sh` and confirmed running in Sierra Chart, same as
Phase A. Both phases are now production-validated, not just clean-rebuild-validated.

## `~/.local` inventory: what else Puget needs (beyond the sysroot above)

Compiled by auditing this machine's actual `~/.local/{bin,lib,share,state,opt}` contents, since
that's the directory this whole migration already lives under. Split into what's actually relevant
to this repo's workflow vs. generic desktop-app cruft that isn't worth porting.

**Relevant — should exist on Puget too:**

| Item | What it is | Puget action |
|---|---|---|
| `~/.local/bin/xwin` | The `xwin` CLI (this doc's own Phase B tool) | Reinstall fresh via the same `gh release download 0.10.0 --repo Jake-Shadle/xwin` method (execution log step 1) — don't copy the binary, just re-fetch, it's a 2-minute no-sudo install |
| `~/.local/bin/claude` (symlink) + `~/.local/share/claude/versions/*` | Claude Code CLI itself | Reinstall via Claude Code's own installer on Puget — don't copy; versions are managed by the installer, a stale copied symlink would point at a version directory that doesn't exist there |

**Found, relevance unclear — confirm before doing anything:**

| Item | What it is | Note |
|---|---|---|
| `~/.local/bin/agy` (210M stripped ELF binary) | Unknown — not referenced anywhere in this repo (`grep`'d, zero hits) | Don't assume it's needed for `MindfulTrader`; likely a personal/unrelated tool. Confirm with the operator before deciding whether Puget needs it. |
| `~/.local/bin/weasyprint` + `~/.local/lib/python3.13/site-packages/{weasyprint,cssselect2,pydyf,pyphen,tinycss2,tinyhtml5,zopfli}` | PDF-generation Python package, `pip --user`-installed under bare `python3.13` (not the `mts`/`atratus` conda envs) | Its launcher script's shebang points at `~/anaconda3/envs/mts/bin/python3.13`, but the package itself lives in the *plain* user site-packages — an unusual split. Not referenced by any build/deploy script in this repo. Confirm whether any workspace tooling (report generation?) actually depends on it before porting. |

**Real side-finding, not `~/.local`-scoped but affects reproducing this machine's setup accurately:**
`cmake` on this machine is **not** the `pip install --user cmake` from `docs/NEW_MACHINE_WSL_SETUP.md`
step 4 (which would land in `~/.local/bin`) — it's actually `/usr/bin/cmake` 4.3.4, installed via
Kitware's official apt repo (`dpkg -S` confirms package `cmake` from `kitware3ubuntu20.04.1`, not a
pip package). `NEW_MACHINE_WSL_SETUP.md`'s documented method would still work (Ubuntu 20.04's own
apt `cmake` really is too old), but doesn't match what's *actually* installed here — worth fixing
that doc's step 4 to document the Kitware-apt-repo method instead, so Puget ends up with a real
system `cmake` matching this machine rather than a `pip`-shimmed one under `~/.local/bin`.

**Not relevant — skip, will regenerate naturally once the corresponding GUI apps are installed/used:**
`~/.local/share/{CMakeTools,GitKrakenCLI,JetBrains,gedit,gk,jupyter,meld,nautilus,tracker,
applications,keyrings,python_keyring,mamba}`, `~/.local/state/{claude,crossnote,gh}` (session/auth
state — Puget should run its own `gh auth login`, never inherit tokens), `~/.local/opt` (empty).

## Open questions

1. ~~Exact `<name>` under `~/.local/sysroots/`~~ — **RESOLVED, confirmed on Puget 2026-09-13**:
   `x86_64-pc-windows-msvc`, matching `toolchain-clang-cl.cmake`'s own `TRIPLE`, is exactly what
   exists on Puget's disk.
2. ~~Does Puget's VS/SDK install (if any survives this migration) still need to match
   `14.44.35207`/`10.0.26100.0`~~ — **RESOLVED**: confirmed Puget has no Visual Studio install at
   all; `xwin` fully retires that requirement there, matching this file's own top-of-doc statement.
3. ~~`xwin` install method on this machine~~ — **RESOLVED for Puget, differs from this machine**:
   confirmed 2026-09-13 that Puget's `xwin` (`~/.cargo/bin/xwin`, v0.10.0) was installed via
   `cargo install xwin` (bookkeeping in `~/.cargo/.crates.toml` confirms it), not the prebuilt
   release-binary method used on this machine — no live Rust toolchain (`cargo`/`rustup`) persists
   on Puget now, and none is needed since the resulting binary runs standalone. Not a blocker; see
   `docs/PUGET_SETUP_COORDINATION.md` Entry 2 §2.5.
4. Should `docs/NEW_MACHINE_WSL_SETUP.md` be rewritten to replace steps 9-11 (Windows-side VS +
   vcpkg + toolchain-file version verification) once this lands, per the Doc Sync Contract?
   — **Agreed yes** (`docs/PUGET_SETUP_COORDINATION.md` Entry 2 ask 4); holding off until the
   Puget-side toolchain fix is applied and a real `./build_dll.sh` succeeds there, not before.
