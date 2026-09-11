# New Machine WSL Setup Guide

Replicates the current WSL2/Ubuntu 20.04 dev environment (this machine) on a new Windows PC, scoped
to what's actually needed: the `mts` and `atratus` conda/mamba environments, the C++ cross-compile
toolchain for `MindfulTrader`'s Windows DLL, and the 4 sibling git repos. Run the commands in order;
sections marked **[Windows]** run in an elevated PowerShell on Windows itself, everything else runs
inside WSL.

## 0. Current machine facts (for reference, verified on this machine 2026-09-09)

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
    software-properties-common gnupg lsb-release ca-certificates
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

If either environment fails to solve exactly (cross-platform/version drift is possible even with
`--no-builds`), fall back to installing the handful of packages this repo's own tools actually
require and letting mamba resolve the rest fresh:

```bash
mamba create -n mts -y python=3.11 arrow-cpp=22 pyarrow=22 nlohmann_json
```

(Verify the exact `arrow`/`parquet` version needed matches this machine's `22.0.0` — check with
`mamba run -n mts pkg-config --modversion arrow parquet` on the OLD machine before relying on a
fresh solve.)

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
