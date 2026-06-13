#!/usr/bin/env bash
set -euo pipefail

required_clang_format_version="clang-format version 21.1.8"

clang_format_candidates=()
if [ -n "${CLANG_FORMAT:-}" ]; then
    clang_format_candidates+=("$CLANG_FORMAT")
fi

if command -v python3 >/dev/null 2>&1; then
    python_user_base=$(python3 -m site --user-base 2>/dev/null || true)
    if [ -n "$python_user_base" ]; then
        clang_format_candidates+=("$python_user_base/bin/clang-format")
    fi
fi

clang_format_candidates+=(
    "clang-format-21"
    "clang-format-21.1"
    "clang-format"
)

clang_format=""
for candidate in "${clang_format_candidates[@]}"; do
    if [ -z "$candidate" ]; then
        continue
    fi
    if [ -x "$candidate" ]; then
        candidate_path="$candidate"
    elif command -v "$candidate" >/dev/null 2>&1; then
        candidate_path=$(command -v "$candidate")
    else
        continue
    fi

    candidate_version=$("$candidate_path" --version 2>/dev/null || true)
    case "$candidate_version" in
        "$required_clang_format_version"*)
            clang_format="$candidate_path"
            break
            ;;
    esac
done

if [ -z "$clang_format" ]; then
    found_version="not found"
    if command -v clang-format >/dev/null 2>&1; then
        found_version=$(clang-format --version)
    fi
    cat >&2 <<EOF
clang-format 21.1.8 is required, found: $found_version

Install the pinned formatter with:
  python3 -m pip install --user clang-format==21.1.8

Or set CLANG_FORMAT to a clang-format 21.1.8 executable.
EOF
    exit 1
fi

crlf_files=$(git grep -Il $'\r' -- . \
    ':(exclude)third_party/**' \
    ':(exclude)ports/host_native/vendor/**' || true)
if [ -n "$crlf_files" ]; then
    echo "Files with CRLF or CR line endings found:" >&2
    echo "$crlf_files" >&2
    exit 1
fi

found=0
while IFS= read -r -d '' file; do
    case "$file" in
        third_party/* | ports/host_native/vendor/*)
            continue
            ;;
        drivers/vga_lcdcam_s3/vga_lcd_rgb_320d.c)
            # Verbatim fork of ESP-IDF's esp_lcd RGB panel driver; kept in
            # upstream style so diffs against IDF stay legible. Not ours to
            # restyle — same treatment as third_party.
            continue
            ;;
    esac
    found=1
    "$clang_format" --dry-run --Werror "$file"
done < <(git ls-files -z -- '*.c' '*.h' '*.cc' '*.cpp' '*.cxx' '*.hh' '*.hpp' '*.hxx')

if [ "$found" -eq 0 ]; then
    echo "No C/C++ files found."
    exit 0
fi
