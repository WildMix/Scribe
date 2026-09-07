#!/usr/bin/env sh
set -eu

if command -v apt-get >/dev/null 2>&1; then
    SUDO=
    if [ "$(id -u)" -ne 0 ]; then
        if ! sudo -n true 2>/dev/null; then
            echo "passwordless sudo is not available; rerun as root or run:" >&2
            echo "  sudo apt-get update" >&2
            echo "  sudo apt-get install -y build-essential cmake pkg-config git libssl-dev python3" >&2
            exit 1
        fi
        SUDO=sudo
    fi
    $SUDO apt-get update
    $SUDO apt-get install -y build-essential cmake pkg-config git libssl-dev python3
else
    echo "install-deps-ubuntu.sh requires apt-get" >&2
    exit 1
fi
