#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR=${SCRIBE_BUILD_DIR:-"$ROOT_DIR/build"}
export SCRIBE_SMOKE_BUILD="$BUILD_DIR"

# Use an isolated store and a dynamically allocated port; never touch user data.
python3 - <<'PY'
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time

build = Path(os.environ["SCRIBE_SMOKE_BUILD"]).resolve()
server_bin, client_bin = build / "scribe", build / "scribe-cli"

def run(*args):
    return subprocess.check_output([str(arg) for arg in args], timeout=30)

with tempfile.TemporaryDirectory(prefix="scribe-smoke-") as root:
    root = Path(root)
    store = root / ".scribe"
    payload = bytes(range(256)) * 256
    source = root / "content.bin"
    source.write_bytes(payload)
    run(server_bin, "init", store)
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
    url = f"http://127.0.0.1:{port}"

    def client(*args):
        return run(client_bin, "--url", url, *args)

    for cycle in range(2):
        with (root / f"server-{cycle}.log").open("wb") as log:
            server = subprocess.Popen([str(server_bin), "--store", str(store),
                                       "--listen", f"127.0.0.1:{port}"], stdout=log, stderr=log)
            try:
                for attempt in range(100):
                    if server.poll() is not None:
                        raise RuntimeError("server exited during startup")
                    try:
                        client("info")
                        break
                    except subprocess.CalledProcessError:
                        time.sleep(0.05)
                else:
                    raise RuntimeError("server did not become ready")
                if cycle == 0:
                    first = json.loads(client("ingest", "--path", "smoke/content",
                                              "--message", "smoke", "--file", source))
                    retry = json.loads(client("ingest", "--path", "smoke/content", "--file", source))
                    assert first == retry, "unchanged retry created a commit"
                assert client("show", "HEAD:smoke/content") == payload, "payload mismatch"
                assert b"smoke" in client("log", "--oneline")
            finally:
                server.terminate()
                try:
                    server.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait()
        run(server_bin, "--store", store, "fsck")
        if cycle == 0:
            run(server_bin, "--store", store, "gc", "--pack")
    logs = (store / "scribe_server.log").read_text()
    assert "request started: method=PUT" in logs
    assert "status=200" in logs
print("SMOKE TEST: PASSED")
PY
