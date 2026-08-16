#!/usr/bin/env python3
"""Push MindfulTrader/config/*.json to the live Sierra Chart config path.

config/ (this repo, git-tracked) is the source of truth; /mnt/c/Trading/config/
is a deployment target. Run manually after editing config/*.json — not wired
into any build step, matching this repo's other un-wired scripts/ entries
(check_nh_nl_freshness.py, refresh_sierra_chart_dependencies.sh).
"""
import argparse
import json
import os
import shutil
import sys
from datetime import datetime, timezone
from pathlib import Path

REPO_CONFIG_DIR = Path(__file__).resolve().parent.parent / "config"
LIVE_CONFIG_DIR = Path("/mnt/c/Trading/config")
CONFIG_FILES = ["execution_params.json", "hmm_regime_risk_policy.json"]


def _utc_stamp() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")


def _backup_file(path: Path) -> Path:
    backup = path.with_suffix(path.suffix + f".bak.{_utc_stamp()}")
    shutil.copy2(path, backup)
    fd = os.open(backup, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)
    return backup


def _write_json_atomic(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + f".tmp.{_utc_stamp()}")
    tmp.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")

    fd = os.open(tmp, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)

    os.replace(tmp, path)
    dir_fd = os.open(path.parent, os.O_RDONLY)
    try:
        os.fsync(dir_fd)
    finally:
        os.close(dir_fd)


def promote(repo_dir: Path, live_dir: Path) -> list:
    """Push every file in CONFIG_FILES from repo_dir to live_dir. Returns backups created."""
    backups = []
    for name in CONFIG_FILES:
        src = repo_dir / name
        dst = live_dir / name
        payload = json.loads(src.read_text(encoding="utf-8"))
        if dst.exists():
            backups.append(_backup_file(dst))
        _write_json_atomic(dst, payload)
        print(f"Promoted {src} -> {dst}")
    return backups


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-dir", type=Path, default=REPO_CONFIG_DIR)
    parser.add_argument("--live-dir", type=Path, default=LIVE_CONFIG_DIR)
    args = parser.parse_args()
    promote(args.repo_dir, args.live_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
