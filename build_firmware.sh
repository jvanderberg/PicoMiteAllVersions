#!/bin/bash
# build_firmware.sh — Build PicoMite firmware for rp2040 and/or rp2350.
#
# Mirrors .github/workflows/firmware.yml so local builds exercise the
# same gate CI enforces on main: same SDK (Raspberry Pi Pico SDK 2.2.0
# at $PICO_SDK_PATH), same CMakeLists.txt COMPILE switching, same
# artifact naming. The SDK is never mutated — PicoMite used to patch
# gpio.c/gpio.h in place, but that requirement was removed on branch
# sdk-patch-removal. If the SDK tree is touched by this script, it's a
# bug.
#
# Usage:
#   ./build_firmware.sh              Build both (rp2040 + rp2350)
#   ./build_firmware.sh rp2040       Build rp2040 only
#   ./build_firmware.sh rp2350       Build rp2350 only
#   PICO_SDK_PATH=... ./build_firmware.sh   Override SDK location
#
# Outputs:
#   build/PicoMite.uf2        (rp2040)
#   build2350/PicoMite.uf2    (rp2350)
#
# Exit code: 0 only if every requested target produces a .uf2.

set -euo pipefail
cd "$(dirname "$0")"

export PICO_SDK_PATH="${PICO_SDK_PATH:-$HOME/pico/pico-sdk}"

# --- preflight --------------------------------------------------------------

for cmd in arm-none-eabi-gcc cmake make; do
    command -v "$cmd" >/dev/null 2>&1 \
        || { echo "error: $cmd not on PATH" >&2; exit 2; }
done

[ -d "$PICO_SDK_PATH" ] \
    || { echo "error: PICO_SDK_PATH does not exist: $PICO_SDK_PATH" >&2; exit 2; }

# --- args -------------------------------------------------------------------

TARGETS=()
if [ $# -eq 0 ]; then
    TARGETS=(rp2040 rp2350)
else
    for arg in "$@"; do
        case "$arg" in
            rp2040|rp2350) TARGETS+=("$arg") ;;
            *) echo "error: unknown target '$arg' (want rp2040 or rp2350)" >&2; exit 2 ;;
        esac
    done
fi

# --- snapshot CMakeLists.txt and always restore it on exit -----------------

# Snapshot the current file (not git-tracked state) so uncommitted edits
# to CMakeLists.txt are preserved across the build. Restored on any exit
# path including Ctrl-C / SIGTERM / unexpected failure.
CMAKE_SNAPSHOT=$(mktemp -t picomite_cmakelists.XXXXXX)
cp CMakeLists.txt "$CMAKE_SNAPSHOT"
trap 'cp "$CMAKE_SNAPSHOT" CMakeLists.txt 2>/dev/null || true; rm -f "$CMAKE_SNAPSHOT"' EXIT INT TERM

# --- flip the active COMPILE target in CMakeLists.txt ---------------------

# BSD/GNU sed portable. Starts from the snapshot each time (not git's
# HEAD) so a user who's hand-edited the file locally doesn't lose work.
set_compile() {
    local target="$1"
    cp "$CMAKE_SNAPSHOT" CMakeLists.txt
    if [ "$target" != PICO ]; then
        sed -i.bak \
            -e 's|^set(COMPILE PICO)$|#set(COMPILE PICO)|' \
            -e "s|^#set(COMPILE ${target})\$|set(COMPILE ${target})|" \
            CMakeLists.txt
        rm -f CMakeLists.txt.bak
    fi
    local active
    active=$(grep -cE '^set\(COMPILE ' CMakeLists.txt || true)
    if [ "$active" -ne 1 ]; then
        echo "error: CMakeLists.txt has $active active COMPILE lines after rewrite (expected 1)" >&2
        exit 2
    fi
}

# --- per-target build ------------------------------------------------------

build_one() {
    local plat="$1" compile build_dir stamp prev jobs
    case "$plat" in
        rp2040) compile=PICO;        build_dir=build     ;;
        rp2350) compile=PICORP2350;  build_dir=build2350 ;;
    esac

    echo
    echo "=== Building $plat (COMPILE=$compile, dir=$build_dir) ==="
    set_compile "$compile"

    # Pico SDK caches PICO_PLATFORM; switching targets needs a fresh
    # configure. We stamp the build dir with its current target and wipe
    # if it differs.
    stamp="$build_dir/.compile_target"
    prev=""
    [ -f "$stamp" ] && prev=$(cat "$stamp")
    if [ "$prev" != "$compile" ]; then
        echo "  cleaning $build_dir (target was '${prev:-<none>}', now '$compile')"
        rm -rf "$build_dir"
        mkdir "$build_dir"
    else
        mkdir -p "$build_dir"
    fi

    (cd "$build_dir" && cmake -DPICO_SDK_PATH="$PICO_SDK_PATH" ..)
    echo "$compile" > "$stamp"

    jobs=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
    make -C "$build_dir" -j"$jobs"

    local uf2="$build_dir/PicoMite.uf2"
    local elf="$build_dir/PicoMite.elf"
    if [ ! -f "$uf2" ]; then
        echo "error: no .uf2 produced for $plat at $uf2" >&2
        exit 1
    fi

    local text
    text=$(arm-none-eabi-size "$elf" | awk 'NR==2 { print $1 }')
    local uf2_size
    uf2_size=$(wc -c < "$uf2" | tr -d ' ')
    echo "  OK: $uf2 (${uf2_size} bytes, text=${text})"
}

for t in "${TARGETS[@]}"; do
    build_one "$t"
done

echo
echo "=== All firmware targets built successfully ==="
