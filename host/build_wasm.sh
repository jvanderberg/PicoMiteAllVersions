#!/bin/bash
# build_wasm.sh — Build the PicoMite WASM target via emscripten.
#
# Usage:
#   ./build_wasm.sh          Build (incremental)
#   ./build_wasm.sh clean    Clean artifacts
#   ./build_wasm.sh rebuild  Clean then build
#
# Output lands in host/web/ as picomite.mjs + picomite.wasm.
#
# Assumes emscripten is on PATH. If not, sources ~/emsdk/emsdk_env.sh when
# present. Otherwise fails with an install hint.

set -e
cd "$(dirname "$0")"

if ! command -v emcc >/dev/null 2>&1; then
    if [ -f "$HOME/emsdk/emsdk_env.sh" ]; then
        # shellcheck disable=SC1091
        source "$HOME/emsdk/emsdk_env.sh" >/dev/null
    fi
fi

if ! command -v emcc >/dev/null 2>&1; then
    echo "ERROR: emcc not found on PATH." >&2
    echo "Install emscripten: https://emscripten.org/docs/getting_started/downloads.html" >&2
    exit 1
fi

case "${1:-}" in
    clean)
        make -f Makefile.wasm clean
        ;;
    rebuild)
        make -f Makefile.wasm clean
        make -f Makefile.wasm
        ;;
    *)
        make -f Makefile.wasm "$@"
        ;;
esac

echo ""
echo "WASM build complete: host/web/picomite.mjs"
echo "Run './web/serve.sh' and open http://localhost:8000/ to smoke test."
