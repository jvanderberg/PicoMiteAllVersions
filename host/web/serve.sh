#!/bin/bash
# serve.sh — Local static server for the PicoMite web bundle.
#
# Usage: ./serve.sh [port]
# Defaults to port 8000.

set -e
cd "$(dirname "$0")"
PORT="${1:-8000}"
echo "Serving $(pwd) at http://localhost:$PORT/"
exec python3 -m http.server "$PORT"
