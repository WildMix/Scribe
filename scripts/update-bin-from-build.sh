#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR=${SCRIBE_BUILD_DIR:-"$ROOT_DIR/build"}
DEST_DIR=${SCRIBE_BIN_DIR:-/bin}

usage() {
    cat <<EOF
Usage: $0 [--build-dir <dir>] [--dest-dir <dir>]

Copies the built Scribe executables into the destination binary directory.

Defaults:
  build dir: \$SCRIBE_BUILD_DIR or $ROOT_DIR/build
  dest dir:  \$SCRIBE_BIN_DIR or /bin
EOF
}

fail() {
    echo "update-bin-from-build: $*" >&2
    exit 1
}

install_binary() {
    src=$1
    dest=$2

    if [ "$(id -u)" -eq 0 ] || [ -w "$DEST_DIR" ]; then
        install -m 0755 "$src" "$dest"
    else
        if ! command -v sudo >/dev/null 2>&1; then
            fail "installing to $DEST_DIR requires root and sudo is not available"
        fi
        sudo install -m 0755 "$src" "$dest"
    fi
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --build-dir)
            [ "$#" -ge 2 ] || fail "missing value for --build-dir"
            BUILD_DIR=$2
            shift 2
            ;;
        --dest-dir)
            [ "$#" -ge 2 ] || fail "missing value for --dest-dir"
            DEST_DIR=$2
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            fail "unknown option '$1'"
            ;;
    esac
done

[ -d "$BUILD_DIR" ] || fail "build directory not found: $BUILD_DIR"
[ -d "$DEST_DIR" ] || fail "destination directory not found: $DEST_DIR"

for exe in scribe scribe-cli; do
    src="$BUILD_DIR/$exe"
    dest="$DEST_DIR/$exe"

    [ -f "$src" ] || fail "built binary not found: $src"
    [ -x "$src" ] || fail "built binary is not executable: $src"
    echo "install $src -> $dest"
    install_binary "$src" "$dest"
done

echo "updated in $DEST_DIR: scribe scribe-cli"
