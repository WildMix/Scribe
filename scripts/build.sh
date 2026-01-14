#!/bin/bash
# Scribe Build Script

set -e

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"

# Parse arguments
BUILD_TYPE="Release"
WITH_POSTGRES=ON
RUN_TESTS=OFF

while [[ $# -gt 0 ]]; do
    case $1 in
        --debug)
            BUILD_TYPE="Debug"
            shift
            ;;
        --no-postgres)
            WITH_POSTGRES=OFF
            shift
            ;;
        --test)
            RUN_TESTS=ON
            shift
            ;;
        --clean)
            echo "Cleaning build directory..."
            rm -rf "${BUILD_DIR}"
            shift
            ;;
        --help)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  --debug        Build with debug symbols"
            echo "  --no-postgres  Build without PostgreSQL support"
            echo "  --test         Run tests after build"
            echo "  --clean        Clean build directory first"
            echo "  --help         Show this help"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

echo "=== Scribe Build ==="
echo "Build type: ${BUILD_TYPE}"
echo "PostgreSQL: ${WITH_POSTGRES}"
echo ""

# Create build directory
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

# Configure
echo "Configuring..."
cmake \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DSCRIBE_WITH_POSTGRES="${WITH_POSTGRES}" \
    -DSCRIBE_BUILD_TESTS="${RUN_TESTS}" \
    "${PROJECT_DIR}"

# Build
echo ""
echo "Building..."
cmake --build . --config "${BUILD_TYPE}" -j$(nproc 2>/dev/null || echo 4)

# Run tests if requested
if [ "${RUN_TESTS}" = "ON" ]; then
    echo ""
    echo "Running tests..."
    ctest --output-on-failure
fi

echo ""
echo "=== Build complete! ==="
echo "Binary: ${BUILD_DIR}/bin/scribe"

# Show usage
echo ""
echo "To run scribe:"
echo "  ${BUILD_DIR}/bin/scribe --help"
echo ""
echo "To install system-wide:"
echo "  cd ${BUILD_DIR} && sudo make install"
