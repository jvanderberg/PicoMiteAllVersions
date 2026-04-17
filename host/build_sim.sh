#!/bin/bash
# build_sim.sh — Build the browser-based PicoCalc simulator binary.
#
# Produces ./mmbasic_sim, a superset of mmbasic_test that embeds Mongoose
# and a background HTTP+WS server. The test harness (mmbasic_test) is
# untouched — `./build.sh` still builds it separately.
#
# Usage:
#   ./build_sim.sh          Build (incremental)
#   ./build_sim.sh clean    Clean and rebuild from scratch
#   ./build_sim.sh rebuild  Same as clean
#
# After building, run with ./run_sim.sh (or see `./mmbasic_sim --help`).

set -e
cd "$(dirname "$0")"

JOBS=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

case "${1:-}" in
    clean|rebuild)
        echo "Cleaning..."
        make clean
        echo "Building simulator..."
        make -j"$JOBS" sim
        ;;
    *)
        make -j"$JOBS" sim
        ;;
esac

echo ""
echo "Build complete: ./mmbasic_sim"
echo "Run './run_sim.sh' to launch the browser simulator."
