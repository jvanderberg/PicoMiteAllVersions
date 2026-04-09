#!/bin/bash
# run_tests.sh — Run all .bas test programs through the MMBasic host build
#
# Usage:
#   ./run_tests.sh                                 Compare both engines (default)
#   ./run_tests.sh --interp                        Run all tests with interpreter only
#   ./run_tests.sh --vm                            Run all tests with bytecode VM only
#   ./run_tests.sh tests/t01_print.bas             Run a single test (compare mode)
#   ./run_tests.sh tests/t01_print.bas --vm        Run a single test with specific engine
#
# Exit code: 0 if all tests pass, 1 if any test fails.

set -e
cd "$(dirname "$0")"

BINARY=./mmbasic_test
if [ ! -x "$BINARY" ]; then
    echo "Binary not found. Building..."
    ./build.sh
fi

# Default to comparison mode (both engines)
MODE=""
SINGLE_FILE=""
PASSED=0
FAILED=0
ERRORS=""

# Parse arguments
if [ $# -ge 1 ]; then
    if [ -f "$1" ] 2>/dev/null; then
        # Single file mode
        SINGLE_FILE="$1"
        MODE="${2:-}"
    else
        MODE="$1"
    fi
fi

run_one_test() {
    local testfile="$1"
    local mode="$2"
    local name
    name=$(basename "$testfile" .bas)

    printf "  %-30s " "$name"

    # Capture output and exit code
    local output
    if output=$($BINARY "$testfile" $mode 2>&1); then
        echo "PASS"
        PASSED=$((PASSED + 1))
        return 0
    else
        echo "FAIL"
        FAILED=$((FAILED + 1))
        ERRORS="${ERRORS}\n--- $name ($mode) ---\n${output}\n"
        return 1
    fi
}

display_mode="compare (interpreter vs VM)"
if [ "$MODE" = "--interp" ]; then
    display_mode="interpreter only"
elif [ "$MODE" = "--vm" ]; then
    display_mode="bytecode VM only"
fi

echo "MMBasic Host Test Runner"
echo "========================"
echo ""

if [ -n "$SINGLE_FILE" ]; then
    echo "Running: $SINGLE_FILE ($display_mode)"
    echo ""
    # For single file, show full output
    $BINARY "$SINGLE_FILE" $MODE
    exit $?
fi

echo "Mode: $display_mode"
echo ""

# Run all test files in sorted order
for testfile in tests/t*.bas; do
    [ -f "$testfile" ] || continue
    run_one_test "$testfile" "$MODE" || true
done

echo ""
echo "Results: $PASSED passed, $FAILED failed"

if [ $FAILED -gt 0 ]; then
    echo ""
    echo "Failures:"
    echo -e "$ERRORS"
    exit 1
fi
