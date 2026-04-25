#!/usr/bin/env sh
set -eu

if ! command -v brew >/dev/null 2>&1; then
    echo "install-deps-macos.sh requires Homebrew" >&2
    exit 1
fi

brew list cmake >/dev/null 2>&1 || brew install cmake
brew list pkg-config >/dev/null 2>&1 || brew install pkg-config
brew list mongo-c-driver >/dev/null 2>&1 || brew install mongo-c-driver
