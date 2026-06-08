#!/usr/bin/env bash
# Build an ESP32-S3 port. Opt-in: ESP-IDF is heavyweight and not part of the
# default host/device build gate.
#
# Each ports/esp32_s3* directory is a self-contained ESP-IDF project and can be
# built directly the standard way:
#   . ~/esp/esp-idf/export.sh
#   cd ports/<port> && idf.py set-target esp32s3 && idf.py build
# This wrapper just adds the repo-wide HAL purity gate and the IDF env.
#
# Usage:
#   ./buildesp32.sh [<port>] [idf.py args...]
#     <port>  directory under ports/ — esp32_s3 (octal) or esp32_s3_quad
#             (quad); or 'all' to build every ports/esp32_s3* project.
#             Defaults to esp32_s3.
#
# Examples:
#   ./buildesp32.sh                       # build ports/esp32_s3
#   ./buildesp32.sh esp32_s3_quad         # build the quad port
#   ./buildesp32.sh esp32_s3 fullclean    # pass args through to idf.py
#   ./buildesp32.sh all                   # build both ESP32-S3 ports
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
        if [ ! -f sdkconfig ]; then
            idf.py set-target esp32s3
        fi
        if [ "$#" -eq 0 ]; then
            idf.py build
        else
            idf.py "$@"
        fi
    )
done
