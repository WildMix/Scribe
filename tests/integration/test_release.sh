#!/usr/bin/env bash
set -euo pipefail
export SCRIBE_RELEASE_SCRIPT="$1"
python3 - <<'PY'
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

script = Path(os.environ["SCRIBE_RELEASE_SCRIPT"]).resolve()
with tempfile.TemporaryDirectory(prefix="scribe-release-test-") as directory:
    root = Path(directory)
    def run(*args, ok=True):
        result = subprocess.run(args, cwd=root, text=True, capture_output=True, timeout=30)
        assert (result.returncode == 0) == ok, result.stdout + result.stderr
        return result.stdout
    run("git", "init", "-q")
    run("git", "config", "user.name", "Release Test")
    run("git", "config", "user.email", "release-test@example.invalid")
    (root / "include/scribe").mkdir(parents=True)
    (root / "CMakeLists.txt").write_text("project(scribe VERSION 1.1.2 LANGUAGES C)\n")
    (root / "include/scribe/scribe.h").write_text('#define SCRIBE_VERSION "1.1.2"\n')
    (root / "README.md").write_text("Current release: scribe version 1.1.2.\n")
    shutil.copyfile(script, root / "push-versioned.sh")
    run("git", "add", ".")
    run("git", "commit", "-qm", "Initial fixture")
    initial = run("git", "rev-parse", "HEAD")
    assert "1.1.2 -> 2.0.0" in run("bash", "push-versioned.sh", "major", "--dry-run")
    assert run("git", "rev-parse", "HEAD") == initial
    (root / "unrelated").write_text("keep staged changes out of releases\n")
    run("git", "add", "unrelated")
    run("bash", "push-versioned.sh", "major", "--no-push", ok=False)
    assert "1.1.2" in (root / "CMakeLists.txt").read_text()
    run("git", "restore", "--staged", "unrelated")
    run("bash", "push-versioned.sh", "major", "--no-push")
    for name in ("CMakeLists.txt", "include/scribe/scribe.h", "README.md"):
        assert "2.0.0" in (root / name).read_text(), name
    changed = run("git", "diff-tree", "--no-commit-id", "--name-only", "-r", "HEAD").splitlines()
    assert set(changed) == {"CMakeLists.txt", "include/scribe/scribe.h", "README.md"}
    assert not run("git", "tag").strip(), "release script must not tag before CI"
print("Release script tests passed")
PY
