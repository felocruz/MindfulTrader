# New Machine WSL Setup Guide

Replicates the current WSL2/Ubuntu 20.04 dev environment (this machine) on a new Windows PC, scoped
to what's actually needed: the `mts` and `atratus` conda/mamba environments, the C++ cross-compile
toolchain for `MindfulTrader`'s Windows DLL, and the 4 sibling git repos. Run the commands in order;
sections marked **[Windows]** run in an elevated PowerShell on Windows itself, everything else runs
inside WSL.

## 0. Current machine facts (outgoing Dell machine, for reference, verified on this machine 2026-09-09)

- OS: Ubuntu 20.04.6 LTS (Focal Fossa), WSL2 kernel
- Clang/LLVM: 22.0.0, from `apt.llvm.org`'s `focal` repo
- CMake: 4.3.4, Ninja: 1.10.0
- Conda: Miniconda/Anaconda3 at `~/anaconda3`, mamba 2.1.0
- MSVC toolset: `14.44.35207`, Windows SDK: `10.0.26100.0` (Visual Studio 2022 Community, Windows side)
- vcpkg: `/mnt/c/Users/<user>/vcpkg` (Windows side, mounted into WSL)
- Sierra Chart data dir: `/mnt/c/SierraChart2/Data/` (Windows side)
- Repos: `MindfulTrader`, `lbrnet`, `MTS`, `schema`, `Atratus` (all GitHub, `felocruz` account;
  `schema`/`Atratus` were local-only with no remote until 2026-09-10, now pushed, both private)

## 0a. [Windows] Connect Wi-Fi / Bluetooth (do this first if the machine isn't online yet)

**Wi-Fi:**
1. Click the network icon in the bottom-right system tray.
2. Select your network from the list, click **Connect**, enter the password.
3. If no networks show up at all, the Wi-Fi adapter driver isn't installed yet — check **Device
   Manager** → "Network adapters" for a missing/unknown wireless device, and install the driver
   from the motherboard/adapter manufacturer's site before continuing.

**Bluetooth** (only needed to pair a peripheral, e.g. mouse/keyboard):
1. Settings → **Bluetooth & devices** → toggle Bluetooth on.
2. **Add device** → **Bluetooth** → select your device from the list → follow the pairing prompt.

## 0b. Target machine hardware (Puget Workstation Ryzen X870E R121-L, ordered 2026-09-11)

| Component | Spec |
|---|---|
| Platform | Puget Workstation Ryzen X870E R121-L |
| Motherboard | ASUS ProArt X870E-Creator WiFi |
| CPU | AMD Ryzen 9 9950X, 4.3GHz base, 16 cores / 32 threads, 170W |
| RAM | 2× Crucial Pro DDR5-5600 UDIMM 48GB = **96GB total** |
| GPU | ASUS GeForce RTX 5080 PRIME OC, 16GB VRAM |
| Storage (primary) | Kingston KC3000 1TB Gen4 M.2 SSD |
| Storage (secondary) | Kingston KC3000 2TB Gen4 M.2 SSD |
| PSU | Super Flower LEADEX VII Gold 1300W |
| Case | Fractal Design Define 7 |
| CPU cooling | Noctua NL-LC1-24 240mm AIO |
| Case fans | PWM-ramping upgrade kit |
| Networking | Integrated Ethernet, WiFi, Bluetooth |
| Sound | Onboard |

**Validated via Puget's own factory benchmark run (CrystalDiskMark/Nbody CUDA/Cinebench 2026/
V-Ray, 2026-09-11)** — recorded here as confirmation the hardware is healthy, not a performance
target for this repo's own workloads:
- Both NVMe drives hit textbook Gen4 numbers: ~6.1–7.4 GB/s sequential, ~1.0 GB/s / 235–260K IOPS
  random (Q32) on both the 1TB and 2TB Kingston KC3000 — directly relevant here since
  `mes_candidates.parquet` alone is 9.4GB and `data/raw/` totals ~49GB.
- GPU host↔device bandwidth (Nbody CUDA): ~54–56 GB/s — confirms the RTX 5080 is seated and
  communicating at expected PCIe bandwidth, the precondition for §0c's GPU bring-up checklist.
- Cinebench 2026: single-thread 543 pts, multi-core 9,062 pts (~16.7× scaling across 16 cores/32
  threads, near-ideal). The single-thread score alone is the more relevant number for this repo's
  own single-threaded code (e.g. `FeatureSaliencyEM.h`'s E/M-step, see below) — a large jump over
  the old Xeon E5-1603 v3 on raw clock/IPC, independent of ever parallelizing anything.
- No anomalies (throttling, degraded lanes, driver instability) surfaced across the full suite
  (storage, GPU rendering, CPU rendering, mixed CPU+GPU render engines).

**Headroom this unlocks vs. the outgoing Dell machine.** Several tools in this repo hardcode
RAM-scarcity assumptions that were real constraints on the old box — confirmed by a real
2026-09-03 OOM incident (4 concurrent `observation_vector_recalibration.cpp` passes over the same
471.9M-row file exhausted RAM with no swap configured and took down the whole VS Code/WSL session).
That tool's own comment sizes its default budget for "3-4 concurrent passes sharing a ~15GB box."
Measured directly on the old machine (`lscpu`/`free -h`, 2026-09-11): Intel Xeon E5-1603 v3 @
2.80GHz, 4 cores / 4 threads (no SMT, 2014-era Haswell-EP), 15GiB RAM, 0B swap, no discrete GPU. At
96GB/16-core-32-thread/RTX 5080, that's roughly **6.4× RAM, 4× cores, 8× threads**, plus a GPU that
didn't exist on this box at all.

**One caveat worth flagging now, not after it causes confusion**: more cores doesn't automatically
speed up everything here. [FeatureSaliencyEM.h](../tools/observation_vector/FeatureSaliencyEM.h)'s
E/M-step loops are plain single-threaded `for` loops (confirmed by the 22-minute wall-clock fit
time for a 500k-observation reservoir on this machine, all on one core) — going to 32 threads
won't shorten that specific fit unless the E-step is explicitly parallelized (it's embarrassingly
parallel over observations, so this would be a cheap win if fit time becomes a bottleneck on
Puget). The thread-count jump mainly helps things already using multiple cores today: parallel
build jobs, concurrent tool passes that used to risk OOM one at a time.

**Also worth doing on Puget, not just inheriting the old default**: configure real swap. The old
machine's 2026-09-03 OOM incident happened with 0B swap configured — worth deciding deliberately
whether Puget should have swap as a safety net even with 6× the RAM, rather than silently carrying
forward "no swap" as an assumption nobody actually chose.

**Not yet acted on — flagged for a deliberate decision, not silently changed here.** A
non-exhaustive list of RAM/CPU-driven defaults and design choices that assumed the old machine's
constraints, worth revisiting once this machine is in service:
- `--max-rss-mb` defaults sized for the old ~15GB ceiling: `observation_vector_recalibration.cpp`
  (3072MB default), `MarketDataReplay.cpp` (3072MB default), and the same flag across
  `activity_clock_bv_comparison.cpp`/`imbalance_clock_manager_ratio_eval.cpp`/
  `imbalance_screen1_hurst_eval.cpp`/`imbalance_work_rate_eval.cpp`/`ImbalanceEntropyDivergenceEval.cpp`
  (4096MB defaults)
- `FeatureSaliencyEval.cpp`'s reservoir-sampling cap (`--max-observations 500000`, a deliberate
  bound because "a genuine EM fit needs random-access passes over the full observation set" —
  spec §7) — worth checking whether the full 274.9M-row set now fits in memory directly instead
  of being subsampled
- The Feature Saliency EM fitter's single-restart k-means++ initialization (its own spec flags
  "consider multiple-restart (best-of-N log-likelihood) fitting" as a deferred mitigation for a
  known local-optima issue — likely deferred for CPU-time reasons, not yet revisited)
- Any tool whose docstring cites avoiding a past OOM incident as its own design rationale (e.g.
  `whole_vector_redundancy_eval.cpp`: "built specifically to avoid an OOM incident this tool family
  has already hit") — the bounded-memory discipline itself is good engineering independent of RAM
  headroom, but the specific numeric caps were tuned to the old ceiling
- The RTX 5080 (16GB VRAM) is new entirely — `lbrnet`'s model training has no documented
  CUDA/GPU-accelerated path today; whether the Student-t HMM's EM fitting or any future Transformer
  training could benefit is an open question, not yet evaluated

None of the above are changed by this edit — recorded here so each gets a deliberate decision once
Puget is in service, not silently inherited from a machine being retired.

**Confirmed gap, 2026-09-13: no `.wslconfig` exists on Puget at all.** WSL2 is running with its
default ~50%-of-host heuristic — `free -h` inside WSL reports **45GiB**, not the 96GB host total,
and `swapon --show` shows 12GB already configured (someone already decided on swap; confirm this
was deliberate and 12GB is the intended size). Given this machine's entire point is RAM headroom
over the old box, `%USERPROFILE%\.wslconfig` (Windows side) should be created explicitly rather
than left to the default, e.g.:
```ini
[wsl2]
memory=80GB
processors=32
swap=16GB
```
sized to leave real headroom for Windows itself (RTX 5080 driver, Sierra Chart, VS Code) rather
than handing WSL2 the whole box. Requires `wsl --shutdown` (Windows side) to take effect — confirm
`free -h` reports the new total afterward before trusting any RAM-headroom-dependent work above.

## 0c. GPU bring-up checklist (RTX 5080, do this after WSL2 is installed — step 1)

**Confirmed on Puget, 2026-09-13: steps 1-4 already pass.** `nvidia-smi` works both in Windows and
inside WSL (driver `610.57.01`, `CUDA UMD Version: 13.3`, RTX 5080 16303MiB) with zero extra Linux
driver install — the passthrough path below is validated, not theoretical. Confirmed Windows 11
Pro (build `10.0.26200.9445`), not Windows 10 — full detail in `docs/PUGET_SETUP_COORDINATION.md`
Entry 4.

The RTX 5080 needs setup on both sides of the WSL2 boundary, in this order:

1. **[Windows] Install/update the NVIDIA GeForce driver** (Studio or Game Ready, either works) —
   this is the ONLY GPU driver needed anywhere in this setup. Reboot after install.
2. **[Windows] Verify**: `nvidia-smi` in a plain PowerShell/CMD window should list the RTX 5080.
3. **[WSL] Do NOT install a Linux NVIDIA driver** — no `sudo apt install nvidia-driver-XXX` or
   similar inside WSL, ever. WSL2 passes the Windows host driver through via a `dxcore`/libcuda
   stub built into the WSL2 kernel itself; a real Linux driver conflicts with that passthrough and
   is a well-known way to break GPU access entirely.
4. **[WSL] Verify**: run `wsl --update` on the Windows side first if this is an older WSL2 install,
   then confirm `nvidia-smi` also works **inside** WSL — it should mirror step 2's output.
5. **[WSL, `mts` env] Install GPU-enabled ML libraries explicitly** — CPU-only wheels are the
   default unless requested: `pytorch-cuda` via the `pytorch`/`nvidia` conda channels, or
   TensorFlow's `[and-cuda]` pip extra (not a bare `pip install tensorflow`, which is exactly
   what `.github/workflows/institutional-backtesting-gate.yml` currently does — check whether that
   resolves a GPU or CPU wheel once this machine exists, don't assume).
   **Confirmed on Puget, 2026-09-13: still the bare CPU wheel** (`pip show tensorflow` on `mts`
   shows zero `nvidia-*` deps). Every stated prerequisite for `tensorflow[and-cuda]` 2.21 is
   already met here (driver `>=525.60.13` ✓ have `610.57.01`; Python 3.10–3.13 ✓ `mts` has 3.13)
   — not yet switched over. Full assessment: `docs/PUGET_SETUP_COORDINATION.md` Entry 4.
6. **Real caveat — Blackwell (`sm_120`) is a very new architecture.** Whatever PyTorch/TensorFlow
   version gets pinned must explicitly ship `sm_120` kernels — check that framework version's own
   release notes before assuming an older pinned build works; a current stable (or nightly, if
   Puget arrives soon after the GPU's own release window) build may be required.
   **Still open on Puget, 2026-09-13**: TensorFlow's own install docs don't explicitly confirm
   `sm_120` support in the 2.21 prebuilt wheel — needs an empirical
   `tf.config.list_physical_devices('GPU')` check once the GPU-enabled extra is installed and the
   `mts` env's GLIBCXX issue (`docs/PUGET_SETUP_COORDINATION.md` Entry 3) is fixed. Don't assume
   either way until that's actually run.
7. **Not solved by the above alone**: enabling GPU-capable libraries doesn't mean training code
   actually dispatches to the GPU. No `device='cuda'`-style dispatch logic was found in `lbrnet`'s
   training scripts as of this writing — that's `lbrnet`-side work (out of `MindfulTrader`'s own
   scope, "no ML training logic belongs in `lbrnet`"), flagged here rather than assumed done.

## 1. [Windows] Install WSL2 + Ubuntu 20.04

**Install to the `C:` drive, not `D:`** -- this setup assumes WSL2's virtual disk (and everything
under it: conda envs, repos, sysroots) lives on `C:`, matching this machine. `wsl --install` uses
`C:` by default (it installs under the Windows user profile), so no special flag is normally
needed -- just don't redirect it to `D:` via `--import`/custom-location options if the installer
or any guide offers that. If Puget's `D:` drive is meant for something else (Sierra Chart data,
bulk storage, etc.), keep that separate from the WSL install itself.

Open PowerShell **as Administrator**:

```powershell
wsl --install -d Ubuntu-20.04
```

Reboot if prompted, then launch "Ubuntu 20.04" from the Start menu once to finish first-time setup
(create your Linux username/password). All remaining steps run **inside that WSL shell** unless
marked **[Windows]**.

## 2. Base Ubuntu packages

```bash
sudo apt update && sudo apt upgrade -y
sudo apt install -y build-essential git curl wget unzip zip pkg-config \
    software-properties-common gnupg lsb-release ca-certificates ripgrep
```

**`ripgrep` (`rg`) is a hard build prerequisite, not optional** — confirmed on Puget 2026-09-13
(`docs/PUGET_SETUP_COORDINATION.md` Entry 2): `MindfulTrader/scripts/audit_shared_root_writes.sh`
(the WS-07 CMake target, runs on every `./build_dll.sh`) calls `rg` directly, and the whole build
fails at step 1/47 without it.

**`gh` (GitHub CLI) needs its own apt repo on Ubuntu 20.04** — confirmed on Puget 2026-09-13
(`apt-cache policy gh` returns no candidate at all from the default `main`/`universe` sources):

```bash
(type -p wget >/dev/null || (sudo apt update && sudo apt-get install wget -y)) \
  && sudo mkdir -p -m 755 /etc/apt/keyrings \
  && out=$(mktemp) && wget -nv -O$out https://cli.github.com/packages/githubcli-archive-keyring.gpg \
  && cat $out | sudo tee /etc/apt/keyrings/githubcli-archive-keyring.gpg > /dev/null \
  && sudo chmod go+r /etc/apt/keyrings/githubcli-archive-keyring.gpg \
  && sudo mkdir -p -m 755 /etc/apt/sources.list.d \
  && echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/githubcli-archive-keyring.gpg] https://cli.github.com/packages stable main" | sudo tee /etc/apt/sources.list.d/github-cli.list > /dev/null \
  && sudo apt update \
  && sudo apt install gh -y
```

## 3. Install Clang/LLVM 22 (cross-compile toolchain)

```bash
wget https://apt.llvm.org/llvm.sh
chmod +x llvm.sh
sudo ./llvm.sh 22 all
# Verify:
clang-cl-22 --version
```

## 4. Install CMake + Ninja

```bash
sudo apt install -y ninja-build
# Ubuntu 20.04's apt CMake is too old (this setup needs 4.x) -- install via pip instead:
sudo apt install -y python3-pip
python3 -m pip install --user cmake
echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.bashrc
echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.zshrc   # confirmed default login shell is zsh, not bash
export PATH="$HOME/.local/bin:$PATH"
cmake --version   # should report 4.x
```

## 5. Install Miniforge (provides `mamba`)

```bash
curl -L -O "https://github.com/conda-forge/miniforge/releases/latest/download/Miniforge3-$(uname)-$(uname -m).sh"
bash Miniforge3-$(uname)-$(uname -m).sh -b -p "$HOME/anaconda3"
"$HOME/anaconda3/bin/conda" init bash
source ~/.bashrc
mamba --version
```

**Required on this machine: the default login shell is `zsh`, confirmed, not `bash`** —
Miniforge's installer only wires `bash` via `conda init bash` above, so a fresh `zsh` terminal has
no `mamba`/`conda` on `PATH` at all until this is also done (a bare `mamba run -n mts ...` fails
with `command not found`; confirmed root cause on Puget 2026-09-13,
`docs/PUGET_SETUP_COORDINATION.md` Entry 1). Do not skip this as merely optional/conditional:

```bash
echo 'eval "$(mamba shell hook --shell zsh)"' >> ~/.zshrc
source ~/.zshrc
mamba --version   # should now also work in this zsh shell -- the one that actually matters here
```

## 6. Recreate the `mts` and `atratus` conda environments

**On the OLD machine**, export both environments first:

```bash
mamba env export -n mts --no-builds > mts-environment.yml
mamba env export -n atratus --no-builds > atratus-environment.yml
```

Copy both `.yml` files to the new machine (via a shared drive, `scp`, USB, etc.), then **on the new
machine**:

```bash
mamba env create -n mts -f mts-environment.yml
mamba env create -n atratus -f atratus-environment.yml
mamba env list   # confirm both exist
```

**On Puget, both envs already exist but are NOT fit for use as-is** (confirmed 2026-09-13,
`docs/PUGET_SETUP_COORDINATION.md` Entries 2-3) — this is a real failure, not a hypothetical to
"validate later":

- `mamba run -n mts pkg-config --modversion arrow parquet` finds **neither package** at all (not
  just a version mismatch from this machine's `22.0.0`).
- `mamba run -n mts python -c "import numpy"` **crashes**:
  `ImportError: .../libstdc++.so.6: version 'GLIBCXX_3.4.29' not found`. Root cause: Ubuntu
  20.04's system `libstdc++.so.6` only provides up to `GLIBCXX_3.4.28`; the `mts` env does carry
  its own newer `libstdcxx-ng 16.2.0` but it isn't being resolved at runtime under `mamba run`
  (`LD_LIBRARY_PATH` empty) — an activation/RPATH problem, not a missing package.
- Installed versions have drifted hard from what's pinned: `numba 0.67.0` vs. `CLAUDE.md`'s
  hard-required `numba==0.62.1`, plus `numpy 2.5.3`/`pyarrow 25.0.0`/`tensorflow 2.21.0` — none of
  which match a lock-file-constrained install, consistent with the env having come from a fresh
  solve rather than `lbrnet/environment-linux-64.lock`.

**Do not build on top of this env as-is.** Recreate `mts` (and check `atratus` for the same drift)
from `lbrnet/environment-linux-64.lock`/`requirements-mts.lock.txt` directly — not
`mts-environment.yml`'s fresh-solve fallback below — then re-run the `pkg-config` +
`import numpy`/`tensorflow` checks as the actual pass gate before trusting it.

If either environment fails to solve exactly (cross-platform/version drift is possible even with
`--no-builds`), fall back to installing the handful of packages this repo's own tools actually
require and letting mamba resolve the rest fresh:

```bash
mamba create -n mts -y python=3.11 arrow-cpp=22 pyarrow=22 nlohmann_json
```

(Verify the exact `arrow`/`parquet` version needed matches this machine's `22.0.0` — check with
`mamba run -n mts pkg-config --modversion arrow parquet` on the OLD machine before relying on a
fresh solve.)

## 6a. If adopting `uv` alongside mamba: strict separation rules (do not skip)

Decided 2026-09-10 (see `lbrnet/logs/rc_gemini.log` line ~7625): `uv` (Astral's Rust-based Python
package manager) is adopted **additively**, for pure-Python `pip`-section installs only — never as
a conda/mamba replacement. Mixing conda and pip/uv installs in one environment is a well-known
source of environment corruption (ABI mismatches, one manager silently upgrading a package the
other thinks it owns) if done carelessly. Rules, not suggestions:

1. **Order matters**: always let `mamba env create`/`mamba install` finish installing every conda
   package FIRST. Only run `uv` afterward, never interleaved.
2. **No overlap, ever**: a package is either conda-managed (in `environment.yml`'s top-level conda
   deps) or `uv`-managed (in `environment.yml`'s `pip:` section) — never both. If `uv` would
   install/upgrade something conda already provides (`numpy`, `scipy`, `pyarrow`, `tensorflow`,
   `arrow-cpp`, anything with a compiled/ABI-sensitive conda-forge build), that's a bug in the
   split, not a normal occurrence — fix the environment file, don't let it happen silently.
3. **Always target the conda env's own interpreter explicitly**: `uv pip install --python
   "$(mamba run -n mts which python)" <packages>` — never bare `uv pip install` (it may resolve a
   different Python than intended) and never `uv venv`/`uv init` inside an active conda env (that
   creates a second, conflicting virtual environment layer).
4. **Re-verify after every `uv` install**: re-run the environment's own lock-integrity check
   (`lbrnet`'s `sha256sum --check environment.lock.sha256` / `Atratus`'s `conda-lock` mechanism) —
   these exist specifically to catch this class of drift immediately, not after the fact.
5. **`--no-deps` where practical** (already the convention in `Atratus`'s own CI: `pip install -e .
   --no-deps`) — prevents `uv` from silently pulling in a transitive dependency conda already
   pins to a specific version.

Concrete first candidate for this pattern: `lbrnet`'s own CI workflow
(`.github/workflows/institutional-backtesting-gate.yml`) currently runs a slow, unpinned `mamba
run -n mts pip install tensorflow jupyterlab opencv-python firebase-admin google-cloud-firestore
google-cloud-storage protobuf grpcio` step on every run (because `setup-micromamba` never
processes `environment.yml`'s own `pip:` section) — this is exactly the kind of pure-Python,
already-isolated-from-conda install `uv` should replace for speed, not yet done as of this writing.

## 7. GitHub authentication

```bash
ssh-keygen -t ed25519 -C "your_email@example.com"
cat ~/.ssh/id_ed25519.pub
```

Add the printed public key to GitHub (Settings → SSH and GPG keys), then verify:

```bash
ssh -T git@github.com
```

## 8. Clone the sibling repos (same layout as this machine: `~/devel/VSCode/`)

```bash
mkdir -p ~/devel/VSCode
cd ~/devel/VSCode
git clone git@github.com:felocruz/MindfulTrader.git
git clone git@github.com:felocruz/lbrnet.git
git clone git@github.com:felocruz/MTS.git
git clone git@github.com:felocruz/schema.git
git clone git@github.com:felocruz/Atratus.git
```

## 9. Cross-compile sysroot (no Visual Studio install needed on this machine)

**Puget-specific caution (2026-09-13): do not follow this step's commands literally yet.** The
`xwin splat` invocation below was validated on the old (now-retired) Dell machine only. Puget's
actual on-disk sysroot uses a differently-shaped splat (`crt/`/`sdk/` flat layout, `x86_64` not
`x64` arch dirs) that the committed `toolchain-clang-cl.cmake` doesn't yet match — a fix is
empirically validated but not yet applied/build-verified. Live status, findings, and the pending
fix: `docs/PUGET_SETUP_COORDINATION.md` (Entries 1-2). This section will be rewritten to match
reality once that fix is confirmed against a real `./build_dll.sh` run, per the Doc Sync Contract.

As of 2026-09-10 the C++ toolchain no longer depends on a Windows-side Visual Studio install or
`/mnt/c` at all -- everything lives natively under `~/.local/sysroots/x86_64-pc-windows-msvc/`.
Full background/rationale: `docs/CROSS_COMPILE_SYSROOT_MIGRATION.md`.

**vcpkg artifacts (zmq/sodium/nlohmann_json):**
```bash
mkdir -p ~/.local/sysroots/x86_64-pc-windows-msvc
cd ~/.local/sysroots/x86_64-pc-windows-msvc
gh release download sysroot-vcpkg-x64-windows-20260910 -R felocruz/MindfulTrader
tar -xzvf mindfultrader-vcpkg-sysroot-x64-windows.tar.gz
rm mindfultrader-vcpkg-sysroot-x64-windows.tar.gz
```

**MSVC CRT + Windows SDK (via `xwin`, no VS installer, no sudo):**
```bash
cd /tmp
gh release download 0.10.0 --repo Jake-Shadle/xwin --pattern "xwin-0.10.0-x86_64-unknown-linux-musl.tar.gz*"
sha256sum xwin-0.10.0-x86_64-unknown-linux-musl.tar.gz   # compare manually against the .sha256 file's bare hash
tar -xzvf xwin-0.10.0-x86_64-unknown-linux-musl.tar.gz
cp xwin-0.10.0-x86_64-unknown-linux-musl/xwin ~/.local/bin/xwin
chmod +x ~/.local/bin/xwin
xwin --accept-license --temp --crt-version 14.44.17.14 --sdk-version 10.0.26100 \
  splat --output ~/.local/sysroots/x86_64-pc-windows-msvc \
  --preserve-ms-arch-notation --use-winsysroot-style
```
(Do **not** add `--disable-symlinks` -- that flag is only correct when running clang-cl *on Windows
itself*; cross-compiling from this Linux/WSL host needs the default casing-fix symlinks, e.g.
`windows.h` -> `Windows.h`, or the build fails with `fatal error: 'windows.h' file not found`.)

`toolchain-clang-cl.cmake`'s `MSVC_VERSION`/`SDK_VERSION` (`14.44.17.14`/`10.0.26100`) are `xwin`'s
own on-disk directory-naming convention, not the real MSVC toolset/SDK version strings -- already
wired up correctly in the committed toolchain file, nothing to edit here unless `xwin` resolves
different versions on this machine (compare via `xwin --accept-license list` first).

## 10. Verify the build

```bash
cd ~/devel/VSCode/MindfulTrader
rm -rf build-windows && ./build_dll.sh
file build-windows/bin/MindfulTrader.dll   # should report: PE32+ executable (DLL) (GUI) x86-64, for MS Windows
```

## 11. (Superseded 2026-09-10, kept for historical reference only)

The old approach -- installing Visual Studio + vcpkg on the Windows side, reading both through
`/mnt/c` -- is no longer used. See step 9 above and `docs/CROSS_COMPILE_SYSROOT_MIGRATION.md` for
why (the `/mnt/c` 9p mount was slow enough to cause real build friction, and Puget won't have a
Visual Studio install at all).

## 12. [Windows] Install Sierra Chart

Install Sierra Chart to `C:\SierraChart2\` (matching `deploy_mindfultrader.sh`'s hardcoded
`DEST_FILE` path), or edit that script's `DEST_FILE` variable if you install it elsewhere.

## 12a. Restore custom chartbooks (do this right after step 12, before first launch)

The 10 custom `.Cht` files (`Elder_DayTrading`, `Elder_HourlyTrading`, `Elder_Swing`,
`Elder_Swing_GOLD`, `Elder_Invest_FXAIX`, `Elder_TripleScreen`, `ES`, `Market_Minder`,
`BacktestingHourlyTrading`, `TurtleSoup`) plus `ChartbookSharingSettings.config` are not in git --
transferred as a GitHub release asset (2026-09-10):

```bash
cd "/mnt/c/SierraChart2/Data"
gh release download sierrachart-chartbooks-20260910 -R felocruz/MindfulTrader
tar -xzvf sierrachart-chartbooks-20260910.tar.gz
rm sierrachart-chartbooks-20260910.tar.gz
```

Sierra Chart's own built-in stock-sample chartbooks (`ExampleChartbook`, `Rockwell`, `HindSight`,
`McClellanOscillator`, `Pivot Points Study Example`, `High Accuracy Spread`,
`DeltaNumberBarsOnP&F_5TickReversal`) reinstall automatically with Sierra Chart itself -- don't need
transferring. Delete the `sierrachart-chartbooks-20260910` release from GitHub once confirmed synced
(it's a one-time transfer artifact, not a real release).

## 13. Copy large data files separately (not via git)

`lbrnet/data/` is **not tracked in git and is large** -- roughly 74GB even after excluding obvious
baseline/temp cruft (verified 2026-09-10):

- `data/scid/` -- 13 raw `.scid` tick files, ~18GB total (the source-of-truth originals)
- `data/raw/` -- `.context`/`.alpha`/`.parquet` files, ~49GB (several multi-GB each, e.g.
  `event_data.context` 14G, `event_data.alpha` 12G, `mes_candidates.parquet` 8.8G,
  `mes_ticks.parquet` 2.7G)
- `data/training/` -- more `.alpha`/`.parquet` pairs, several hundred MB each

This is too large for GitHub releases or casual copying -- needs an external drive or a direct
network link between the two machines. Before moving all of it, consider whether some of it is
regenerable instead (the `.scid` files are the true originals; `tools/market_data_replay/` in
`MindfulTrader` can reconstruct `.context`-equivalent output from raw ticks, so not every derived
file may need to make the trip). Once you've decided what to bring:

```bash
mkdir -p ~/devel/VSCode/lbrnet/data/raw
# then copy the needed files into that directory (and data/scid/, data/training/ as needed)
```

## 14. [Windows] Install VS Code + extensions

Install VS Code, then the **WSL** extension (`ms-vscode-remote.remote-wsl`) and **GitHub Copilot**
(`GitHub.copilot` + `GitHub.copilot-chat`). Open the repo from WSL:

```bash
cd ~/devel/VSCode/MindfulTrader
code .
```

## 15. Verify the full build

```bash
cd ~/devel/VSCode/MindfulTrader
bash /home/rcruz/devel/VSCode/scripts/regenerate_schema.sh   # if this path differs, adjust first
./build_dll.sh
```

A successful run ends with `Build Successful!` and produces
`build-windows/bin/MindfulTrader.dll`. If this fails, the error will point at either a missing
`mts`-env package (step 6) or a toolchain path mismatch (step 11) — those are the two most likely
gaps on a fresh machine.
