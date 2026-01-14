#!/bin/bash
# Scribe Development Environment Setup
# Run this script to install dependencies

set -e

echo "=== Scribe Development Setup ==="
echo ""

# Detect OS
if [ -f /etc/debian_version ]; then
    echo "Detected Debian/Ubuntu system"
    echo "Installing dependencies with apt..."
    sudo apt-get update
    sudo apt-get install -y \
        build-essential \
        cmake \
        libssl-dev \
        libsqlite3-dev \
        libpq-dev

elif [ -f /etc/redhat-release ]; then
    echo "Detected Red Hat/CentOS system"
    echo "Installing dependencies with yum/dnf..."
    sudo dnf install -y \
        gcc \
        cmake \
        openssl-devel \
        sqlite-devel \
        libpq-devel

elif [ "$(uname)" == "Darwin" ]; then
    echo "Detected macOS"
    echo "Installing dependencies with Homebrew..."
    brew install cmake openssl sqlite libpq

else
    echo "Unknown OS. Please install manually:"
    echo "  - cmake"
    echo "  - OpenSSL development headers"
    echo "  - SQLite3 development headers"
    echo "  - libpq (PostgreSQL client) development headers"
    exit 1
fi

echo ""
echo "=== Dependencies installed successfully! ==="
echo ""
echo "To build Scribe:"
echo "  mkdir build && cd build"
echo "  cmake .."
echo "  make"
echo ""
echo "To install system-wide:"
echo "  sudo make install"
