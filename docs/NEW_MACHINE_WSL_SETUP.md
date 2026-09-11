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
- Repos: `MindfulTrader`, `lbrnet`, `MTS`, `schema` (all GitHub, `felocruz` account; `schema` was
  local-only with no remote until 2026-09-10, now pushed to `felocruz/schema`, private)

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
```

## 9. [Windows] Visual Studio 2022 + Windows SDK (needed for the cross-compile toolchain)

Install Visual Studio 2022 Community with the **"Desktop development with C++"** workload (includes
the MSVC toolset and a Windows SDK). Match versions if possible:

- MSVC toolset: `14.44.35207`
- Windows SDK: `10.0.26100.0`

Via the VS Installer, use "Individual components" to pin these exact versions if the new machine's
default differs — `toolchain-clang-cl.cmake`'s `MSVC_VERSION`/`SDK_VERSION` variables must match
whatever actually gets installed (see step 11).

## 10. [Windows] Install vcpkg

```powershell
cd C:\Users\<your-username>
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg
.\bootstrap-vcpkg.bat
```

## 11. Verify/update the toolchain file

After steps 9-10, confirm the installed MSVC toolset and SDK versions match what
`toolchain-clang-cl.cmake` expects:

```bash
ls "/mnt/c/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/"
ls "/mnt/c/Program Files (x86)/Windows Kits/10/Include/"
```

If the version strings differ from `14.44.35207`/`10.0.26100.0`, edit
`~/devel/VSCode/MindfulTrader/toolchain-clang-cl.cmake`'s `MSVC_VERSION`/`SDK_VERSION` at the top of
the file to match the new machine's actual installed versions.

## 12. [Windows] Install Sierra Chart

Install Sierra Chart to `C:\SierraChart2\` (matching `deploy_mindfultrader.sh`'s hardcoded
`DEST_FILE` path), or edit that script's `DEST_FILE` variable if you install it elsewhere.

## 13. Copy large data files separately (not via git)

`lbrnet/data/raw/mes_ticks.parquet` and any `.context`/`.alpha` files are multi-GB and not tracked
in git. Copy them directly (network share, external drive, or `scp`/`rsync` over the LAN) into the
same relative path, e.g.:

```bash
mkdir -p ~/devel/VSCode/lbrnet/data/raw
# then copy mes_ticks.parquet (and any other needed data files) into that directory
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
