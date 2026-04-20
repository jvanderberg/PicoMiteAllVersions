#!/bin/bash
# build_firmware.sh — Build PicoMite firmware for any device target.
#
# Mirrors .github/workflows/firmware.yml so local builds exercise the
# same gate CI enforces on main: same Pico SDK 2.2.0 at $PICO_SDK_PATH,
# same CMakeLists.txt COMPILE switching, same artifacts. The SDK is
# never mutated — the historical gpio.c/gpio.h patches were eliminated
# on the sdk-patch-removal branch. If this script touches the SDK tree,
# it's a bug.
#
# Targets:
#   rp2040        PICO          PicoCalc RP2040 (console + SD)         — default gate
#   rp2350        PICORP2350    PicoCalc RP2350 (pimoroni_pga2350)     — default gate
#   rp2040-web    WEB           RP2040 + cyw43 WiFi (pico_w)           — opt-in, known broken at HEAD
#   rp2350-web    WEBRP2350     RP2350 + cyw43 WiFi (pico2_w)          — opt-in, known broken at HEAD
#
# The WEB variants are known-broken on this fork (pre-existing, unrelated to
# sdk-patch-removal): GUI.c is compiled without GUICONTROLS, vm_sys_graphics.c
# references SPI-LCD types that aren't visible, etc. They are left as explicit
# targets you can pass on the command line so they can be debugged later, but
# are not part of the default gate until a separate branch repairs them.
#
# Usage:
#   ./build_firmware.sh                        Build the default gate (rp2040 + rp2350)
#   ./build_firmware.sh rp2040                 Build one target
#   ./build_firmware.sh rp2040 rp2350          Build a subset
#   ./build_firmware.sh rp2350-web             Opt into a known-broken WEB target
#   PICO_SDK_PATH=... ./build_firmware.sh      Override SDK path (default $HOME/pico/pico-sdk)
#
# Outputs:
#   build/PicoMite.uf2              (rp2040)
#   build2350/PicoMite.uf2          (rp2350)
#   build_web/PicoMite.uf2          (rp2040-web, if it builds)
#   build2350_web/PicoMite.uf2      (rp2350-web, if it builds)
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

# --- target registry --------------------------------------------------------

target_to_compile() {
    case "$1" in
        rp2040)      echo PICO         ;;
        rp2040-web)  echo WEB          ;;
        rp2350)      echo PICORP2350   ;;
        rp2350-web)  echo WEBRP2350    ;;
        *)           echo "" ;;
    esac
}

target_to_dir() {
    case "$1" in
        rp2040)      echo build         ;;
        rp2040-web)  echo build_web     ;;
        rp2350)      echo build2350     ;;
        rp2350-web)  echo build2350_web ;;
        *)           echo "" ;;
    esac
}

# --- args -------------------------------------------------------------------

TARGETS=()
if [ $# -eq 0 ]; then
    # Default gate: targets known to build. Opt into WEB variants explicitly.
    TARGETS=(rp2040 rp2350)
else
    for arg in "$@"; do
        if [ -z "$(target_to_compile "$arg")" ]; then
            echo "error: unknown target '$arg' (want: rp2040, rp2040-web, rp2350, rp2350-web)" >&2
            exit 2
        fi
        TARGETS+=("$arg")
    done
fi

# --- snapshot CMakeLists.txt; always restore on exit ----------------------

# Snapshot the current file (not git's HEAD) so uncommitted edits are
# preserved. Restored on any exit path — Ctrl-C, SIGTERM, failure.
CMAKE_SNAPSHOT=$(mktemp -t picomite_cmakelists.XXXXXX)
cp CMakeLists.txt "$CMAKE_SNAPSHOT"
trap 'cp "$CMAKE_SNAPSHOT" CMakeLists.txt 2>/dev/null || true; rm -f "$CMAKE_SNAPSHOT"' EXIT INT TERM

# --- flip active COMPILE target via sed ------------------------------------

# Mirrors .github/workflows/firmware.yml: comment out ANY active
# set(COMPILE X) line, then uncomment the one we want. BSD/GNU portable.
set_compile() {
    local target="$1"
    cp "$CMAKE_SNAPSHOT" CMakeLists.txt
    # Comment out every existing active line.
    sed -i.bak -E 's|^[[:space:]]*set\(COMPILE [A-Z0-9]+\)|#&|' CMakeLists.txt
    rm -f CMakeLists.txt.bak
    # Uncomment the target. Portable replacement for GNU sed's
    # "0,/pattern/s//repl/" range addressing.
    awk -v tgt="$target" '
        !done && $0 ~ "^#set\\(COMPILE " tgt "\\)" {
            sub(/^#/, "")
            done = 1
        }
        { print }
    ' CMakeLists.txt > CMakeLists.txt.tmp
    mv CMakeLists.txt.tmp CMakeLists.txt

    local active
    active=$(grep -cE '^set\(COMPILE ' CMakeLists.txt || true)
    if [ "$active" -ne 1 ]; then
        echo "error: CMakeLists.txt has $active active COMPILE lines after rewrite (expected 1 for $target)" >&2
        exit 2
    fi
}

# --- per-target build ------------------------------------------------------

build_one() {
    local name="$1" compile build_dir stamp prev jobs
    compile=$(target_to_compile "$name")
    build_dir=$(target_to_dir "$name")

    echo
    echo "=== Building $name (COMPILE=$compile, dir=$build_dir) ==="
    set_compile "$compile"

    # Pico SDK caches PICO_PLATFORM in the build dir's CMakeCache; wipe
    # if the active target has changed since the last configure.
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
        echo "error: no .uf2 produced for $name at $uf2" >&2
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
