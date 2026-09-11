# Puget Workstation New-Machine Bring-Up Spec

## 1. Origin

Consolidates the two open docs from this session — `docs/NEW_MACHINE_WSL_SETUP.md` (full WSL2 dev
environment setup guide, updated 2026-09-11 with the Puget Workstation's real hardware/benchmarks)
and `docs/CROSS_COMPILE_SYSROOT_MIGRATION.md` (Phases A/B of the `/mnt/c`→native sysroot migration,
DONE and validated on the outgoing machine, remaining work explicitly Puget-side) — into one
ordered, gated bring-up sequence for the physical Puget Workstation Ryzen X870E R121-L that arrived
2026-09-11.

**Operator directive (2026-09-11): no application code changes until this machine is ready.** This
spec is the gate for that — it does not itself modify any source file in this repo.

## 2. Scope and non-goals

- **Scope:** everything needed to reach a working `MindfulTrader.dll` build + live Sierra Chart
  validation on Puget, matching the outgoing machine's dev environment.
- **Explicit gate, not a suggestion:** `docs/superpowers/specs/2026-09-11-puget-workstation-
  capacity-headroom-spec.md` and its companion plan (already drafted, ready to execute) may **not**
  begin until this spec's §7 acceptance criteria are met. That work assumes a working Puget build
  to validate against — starting it earlier risks conflating environment-setup problems with code
  problems.
- **Non-goal:** this document does not restate every command from the two source docs — it
  sequences and gates them into phases with clear pass/fail checkpoints. Always read the cited
  section in the source doc for the exact commands before executing a phase.
- **Non-goal:** no code change of any kind in this repo, including doc-sync-contract mirrors,
  is part of this spec.

## 3. Ownership split (who executes what)

- **This session cannot execute anything on Puget** — it runs on the outgoing WSL machine, with no
  shell access to the new hardware. Every phase below is written for the **operator** (or a future
  agent session actually running inside Puget's own WSL instance) to execute.
- **[Windows-side, operator only]**: physical setup, Wi-Fi/Bluetooth pairing, WSL2/Ubuntu install,
  NVIDIA GeForce driver install, Sierra Chart install, VS Code install — no WSL shell exists yet at
  this point, so none of this can be delegated to an agent.
- **[WSL-side, operator or a Puget-resident agent]**: everything from Phase 1 onward, once a WSL
  shell is available.

## 4. Ordered bring-up sequence (gated phases)

### Phase 0 — Physical + Windows host bring-up
**Ref:** `NEW_MACHINE_WSL_SETUP.md` §0a (Wi-Fi/Bluetooth), §0c steps 1-2 (NVIDIA GeForce driver
install + Windows-side `nvidia-smi` verify), §1 (`wsl --install -d Ubuntu-20.04`, installed to
`C:`, not `D:`).
**Gate:** Ubuntu 20.04 launches from the Start menu and completes first-time user setup;
`nvidia-smi` lists the RTX 5080 in a plain Windows PowerShell/CMD window.

### Phase 1 — Base WSL environment
**Ref:** §2 (apt base packages), §3 (Clang/LLVM 22 via `apt.llvm.org`), §4 (Ninja + pip-installed
CMake 4.x), §5 (Miniforge/mamba).
**Also do here (cheap, non-blocking):** configure WSL2 swap (16-32GB) per the capacity-headroom
spec's §4d recommendation — a `.wslconfig` edit, unrelated to any later phase, convenient to do
while other installs run.
**Gate:** `clang-cl-22 --version`, `cmake --version` (reports 4.x, not the stale Ubuntu 20.04 apt
version), and `mamba --version` all succeed.

### Phase 2 — GPU passthrough verification
**Ref:** §0c steps 3-4 only. **Do NOT** install a Linux-side NVIDIA driver inside WSL (`dxcore`
passthrough handles this; a real Linux driver breaks it).
**Gate:** `nvidia-smi` run *inside* WSL mirrors Phase 0's Windows-side output.
**Explicitly deferred, not this spec's scope:** §0c steps 5-7 (GPU-enabled PyTorch/TensorFlow
install, `sm_120` Blackwell kernel compatibility check, actual `device='cuda'` training dispatch)
belong to `lbrnet`'s own session per this repo's "no ML training logic" boundary rule — flag as
outstanding, do not implement here.

### Phase 3 — Conda environments
**Ref:** §6 (recreate `mts`/`atratus` from the outgoing machine's exported `.yml` files, with a
documented fallback fresh-solve command if either fails to solve exactly), §6a (if `uv` is adopted
alongside mamba: strict ordering/no-overlap/`--no-deps` rules — decide whether to adopt it *before*
this phase, not mid-setup).
**Gate:** `mamba env list` shows both `mts` and `atratus`; `mamba run -n mts pkg-config
--modversion arrow parquet` matches the outgoing machine's `22.0.0` (needed for
`tools/`'s Arrow/Parquet-linked utilities).

### Phase 4 — GitHub auth + repo clone
**Ref:** §7 (`ssh-keygen`, add public key to GitHub), §8 (clone all 5 sibling repos — `MindfulTrader`,
`lbrnet`, `MTS`, `schema`, `Atratus` — into `~/devel/VSCode/`, matching the outgoing machine's
layout).
**Gate:** `ssh -T git@github.com` succeeds; all 5 repos present at the expected paths.

### Phase 5 — Cross-compile sysroot
**Ref:** `NEW_MACHINE_WSL_SETUP.md` §9 (summary + exact commands), full rationale and execution
history in `CROSS_COMPILE_SYSROOT_MIGRATION.md`'s "Phase A execution log" step 6 and "Phase B
execution log" steps 1-7.
- **5a.** Download + extract the `sysroot-vcpkg-x64-windows-20260910` GitHub Release asset into
  `~/.local/sysroots/x86_64-pc-windows-msvc/vcpkg/x64-windows/`.
- **5b.** Install `xwin` 0.10.0 as a prebuilt static binary (no Rust/cargo toolchain) to
  `~/.local/bin/xwin`.
- **5c. Before running the pinned splat command**, run `xwin --accept-license list` and confirm it
  still resolves CRT `14.44.17.14` / SDK `10.0.26100` (the versions already hardcoded into
  `toolchain-clang-cl.cmake`'s `MSVC_VERSION`/`SDK_VERSION`). If `xwin`'s default "latest" manifest
  has moved on, either pass the same explicit `--crt-version`/`--sdk-version` pins the command
  already specifies (safe either way) or treat a mismatch as a real finding to report, not a
  silent fix.
- **5d.** Run the exact pinned splat command (`--preserve-ms-arch-notation --use-winsysroot-style`,
  **no** `--disable-symlinks` — that flag is only correct for running clang-cl on Windows itself,
  not this Linux cross-compile host).
**Gate:** `~/.local/sysroots/x86_64-pc-windows-msvc/` contains all three of `vcpkg/x64-windows/`,
`VC/Tools/MSVC/14.44.17.14/`, and `Windows Kits/10/{Include,Lib}/10.0.26100/`.

### Phase 6 — Build verification (the hard gate for all future coding work)
**Ref:** §10 / §15.
```bash
cd ~/devel/VSCode/MindfulTrader
rm -rf build-windows && ./build_dll.sh
file build-windows/bin/MindfulTrader.dll   # expect: PE32+ executable (DLL) (GUI) x86-64, for MS Windows
```
**Gate:** build succeeds end-to-end with **zero** edits to `CMakeLists.txt` or
`toolchain-clang-cl.cmake` beyond what's already committed. Any edit required here is itself a
real finding — report it, do not silently patch and move on.

### Phase 7 — Sierra Chart + chartbooks
**Ref:** §12 (install to `C:\SierraChart2\`, matching `deploy_mindfultrader.sh`'s hardcoded
`DEST_FILE`), §12a (restore the 10 custom `.Cht` chartbooks + `ChartbookSharingSettings.config`
from the `sierrachart-chartbooks-20260910` GitHub Release asset).
**Gate:** Sierra Chart launches with all 10 custom chartbooks present and loadable.

### Phase 8 — Deploy + live validation
**Ref:** `./deploy_mindfultrader.sh`.
**Gate:** the Phase 6 DLL loads inside Sierra Chart without error — matches the "validated live"
bar already met on the outgoing machine per `CROSS_COMPILE_SYSROOT_MIGRATION.md`.

### Phase 9 — Large data transfer (non-blocking, can run in parallel with any phase above)
**Ref:** §13 — `lbrnet/data/` (~74GB: `data/scid/` ~18GB, `data/raw/` ~49GB, `data/training/` a
few hundred MB) is not git-tracked and too large for a GitHub Release; needs an external drive or
direct network link. Consider whether any of it is regenerable instead (`.scid` files are the true
originals; `tools/market_data_replay/` in this repo can reconstruct `.context`-equivalent output
from raw ticks) before deciding what actually needs to make the trip.
**Not a gate** for Phases 6-8 or for starting the capacity-headroom plan — only training/backtesting
work needs this data, not a DLL build/deploy.

### Phase 10 — VS Code + AI tooling
**Ref:** §14 (VS Code + `ms-vscode-remote.remote-wsl` + `GitHub.copilot`/`GitHub.copilot-chat`).
Also install `yzhang.markdown-all-in-one` and `davidanson.vscode-markdownlint` (recommended this
session) given how much of this repo's active work lives in markdown specs/plans.

## 5. Decisions already made — do not re-litigate

Carried over from `CROSS_COMPILE_SYSROOT_MIGRATION.md`, all confirmed 2026-09-10:
- `xwin` fully retires the Windows-side Visual Studio Installer requirement — do **not** install
  Visual Studio on Puget at all.
- vcpkg artifacts (zmq/sodium/nlohmann_json) are a straight copy via GitHub Release asset, never
  rebuilt from source on Puget.
- CRT/SDK come from a **fresh `xwin splat` run performed natively on Puget** (not a transferred
  tarball) — deterministic per pinned version, and avoids transferring ~630MB uncompressed.

## 6. Explicitly deferred until this spec's gate passes

- `docs/superpowers/specs/2026-09-11-puget-workstation-capacity-headroom-spec.md` and its
  companion plan (`docs/superpowers/plans/2026-09-11-puget-workstation-capacity-headroom-
  implementation.md`) — both fully drafted and ready, but Task 1 of the plan must not start before
  Phase 6's gate passes.
- Any other application code change in this repo, per the 2026-09-11 operator directive.

## 7. Acceptance / Done criteria

- Phases 0 through 8 each pass their stated gate, in order.
- No `CMakeLists.txt`/`toolchain-clang-cl.cmake` edit was needed beyond what's already committed
  (or, if one was needed, it's been reported and reviewed before being treated as routine).
- Phase 9's transfer status is recorded either way (in-progress/not-started is fine — it's
  non-blocking).
- Once Phase 6's gate passes, this spec is DONE and the capacity-headroom plan may begin.

## 8. Open items to confirm once Puget is actually in hand

- Phase 5c's version-match check — confirm before trusting the pinned `xwin splat` command as-is.
- Whether `uv` (§6a) is actually adopted for this machine's `mts`/`atratus` envs — decide before
  Phase 3, not mid-setup.
