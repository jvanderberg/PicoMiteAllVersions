#!/bin/bash
# build.sh — Build the MMBasic host test binary
#
# Usage:
#   ./build.sh          Build (incremental)
#   ./build.sh clean    Clean and rebuild from scratch
#   ./build.sh rebuild  Same as clean
#
# The output binary is ./mmbasic_test

set -e
cd "$(dirname "$0")"

case "${1:-}" in
    clean|rebuild)
        echo "Cleaning..."
        make clean
        echo "Building..."
        make -j$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
        ;;
    *)
        make -j$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
        ;;
esac

echo ""
echo "Build complete: ./mmbasic_test"
echo "Run './run_tests.sh' to execute the test suite."
