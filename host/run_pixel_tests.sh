#!/bin/bash
# run_pixel_tests.sh -- Run host graphics regressions with framebuffer assertions

set -e
cd "$(dirname "$0")"

BINARY=./mmbasic_test
if [ ! -x "$BINARY" ]; then
    echo "Binary not found. Building..."
    ./build.sh
fi

run_case() {
    local label="$1"
    local testfile="$2"
    shift 2

    echo "Running pixel test: $label"
    "$BINARY" "$testfile" --interp "$@"
    "$BINARY" "$testfile" --vm "$@"
    echo ""
}

run_case \
    "t60_fastgfx_pixels" \
    "tests/t60_fastgfx_pixels.bas" \
    --assert-pixel 12,12,000000 \
    --assert-pixel 32,12,00FF00 \
    --assert-pixel 15,30,000000 \
    --assert-pixel 40,30,FF0000 \
    --assert-pixel 11,50,000000 \
    --assert-pixel 31,50,FFFFFF

run_case \
    "t61_blocks_screen_pixels" \
    "tests/t61_blocks_screen_pixels.bas" \
    --assert-pixel 0,0,000000 \
    --assert-pixel 6,3,FFFFFF \
    --assert-pixel 99,78,FF0000 \
    --assert-pixel 138,94,FFFF00 \
    --assert-pixel 160,191,FF0000 \
    --assert-pixel 135,223,00FF00
