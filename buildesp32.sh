#!/usr/bin/env bash
# Build an ESP32 port. Opt-in: ESP-IDF is heavyweight and not part of the
# default host/device build gate.
#
# Each ports/esp32_* directory is a self-contained ESP-IDF project and can be
# built directly the standard way:
#   . ~/esp/esp-idf/export.sh
#   cd ports/<port> && idf.py set-target <chip> && idf.py build
# This wrapper just adds the repo-wide HAL purity gate, the IDF env, and the
# per-port chip target (esp32_cyd -> esp32, esp32_s3* -> esp32s3).
#
# Usage:
#   ./buildesp32.sh [<port>] [idf.py args...]
#     <port>  directory under ports/ — esp32_s3 (octal), esp32_s3_quad
#             (quad), or esp32_cyd (classic ESP32); or 'all' to build every
#             ports/esp32_* project. Defaults to esp32_s3.
#
# Examples:
#   ./buildesp32.sh                       # build ports/esp32_s3
#   ./buildesp32.sh esp32_s3_quad         # build the quad port
#   ./buildesp32.sh esp32_cyd             # build the classic-ESP32 CYD port
#   ./buildesp32.sh esp32_s3 fullclean    # pass args through to idf.py
#   ./buildesp32.sh all                   # build every ESP32 port
#
# Environment:
#   IDF_PATH=/path/to/esp-idf    # defaults to ~/esp/esp-idf
#   SKIP_HAL_PURITY=1            # skip the source-level purity gate

set -euo pipefail

root="$(cd "$(dirname "$0")" && pwd)"
idf_path="${IDF_PATH:-$HOME/esp/esp-idf}"

if [ "$#" -gt 0 ]; then
    port="$1"
    shift
else
    port="esp32_s3"
fi

ports=()
if [ "$port" = "all" ]; then
    for d in "$root"/ports/esp32_s3*; do
        [ -f "$d/CMakeLists.txt" ] && ports+=("$(basename "$d")")
    done
else
    ports=("$port")
fi

# Guard the empty case explicitly: iterating an empty array under `set -u`
# aborts with "unbound variable" on older bash (e.g. macOS's 3.2).
if [ "${#ports[@]}" -eq 0 ]; then
    echo "No ESP32 ports found under ports/esp32_s3*." >&2
    exit 2
fi

if [ "${SKIP_HAL_PURITY:-0}" != "1" ]; then
    printf '=== HAL purity gate ===\n'
    "$root/tools/check_hal_purity.sh"
fi

if ! command -v idf.py >/dev/null 2>&1; then
    if [ ! -f "$idf_path/export.sh" ]; then
        echo "ESP-IDF not found. Set IDF_PATH or install ESP-IDF at $idf_path." >&2
        exit 2
    fi
    # export.sh intentionally mutates PATH, IDF_PATH, and toolchain vars.
    # shellcheck disable=SC1090
    . "$idf_path/export.sh" >/dev/null
fi

for p in "${ports[@]}"; do
    port_dir="$root/ports/$p"
    if [ ! -d "$port_dir" ]; then
        echo "No such port: ports/$p" >&2
        exit 2
    fi
    printf '=== Building ports/%s ===\n' "$p"
    (
        cd "$port_dir"
        case "$p" in
            esp32_cyd) chip="esp32" ;;
            *) chip="esp32s3" ;;
        esac
        if [ ! -f sdkconfig ]; then
            idf.py set-target "$chip"
        fi
        if [ "$#" -eq 0 ]; then
            idf.py build
        else
            idf.py "$@"
        fi
    )
done
