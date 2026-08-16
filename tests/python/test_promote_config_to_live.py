"""Test scripts/promote_config_to_live.py's atomic-write + backup behavior."""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent / "scripts"))
import promote_config_to_live as promote_mod  # noqa: E402


def test_promote_writes_content_byte_for_byte(tmp_path):
    repo_dir = tmp_path / "config"
    live_dir = tmp_path / "live"
    repo_dir.mkdir()
    live_dir.mkdir()
    payload = {"_version": "1.1.0", "key": "value"}
    (repo_dir / "execution_params.json").write_text(json.dumps(payload))
    (repo_dir / "hmm_regime_risk_policy.json").write_text(json.dumps({"policy_version": "1.0.0"}))

    promote_mod.promote(repo_dir, live_dir)

    written = json.loads((live_dir / "execution_params.json").read_text())
    assert written == payload


def test_promote_backs_up_existing_live_file(tmp_path):
    repo_dir = tmp_path / "config"
    live_dir = tmp_path / "live"
    repo_dir.mkdir()
    live_dir.mkdir()
    (repo_dir / "execution_params.json").write_text(json.dumps({"_version": "1.1.0"}))
    (repo_dir / "hmm_regime_risk_policy.json").write_text(json.dumps({"policy_version": "1.0.0"}))
    (live_dir / "execution_params.json").write_text(json.dumps({"_version": "old"}))
    (live_dir / "hmm_regime_risk_policy.json").write_text(json.dumps({"policy_version": "old"}))

    backups = promote_mod.promote(repo_dir, live_dir)

    assert len(backups) == 2
    for backup in backups:
        assert backup.exists()
        assert ".bak." in backup.name


if __name__ == "__main__":
    import pytest
    sys.exit(pytest.main([__file__, "-v"]))
