#!/bin/bash
# run_bench.sh — Run benchmarks comparing interpreter vs bytecode VM
#
# Usage:
#   ./run_bench.sh                 Run all benchmarks
#   ./run_bench.sh mandelbrot      Run a specific benchmark
#
# Runs each benchmark through both engines and reports wall-clock times.

set -e
cd "$(dirname "$0")"

BINARY=./mmbasic_test
if [ ! -x "$BINARY" ]; then
    echo "Binary not found. Building..."
    ./build.sh
fi

run_bench() {
    local testfile="$1"
    local name
    name=$(basename "$testfile" .bas | sed 's/bench_//')

    printf "%-20s " "$name"

    # Time interpreter
    local t_interp
    t_interp=$( { time $BINARY "$testfile" --interp >/dev/null 2>&1; } 2>&1 )
    local interp_real
    interp_real=$(echo "$t_interp" | grep real | awk '{print $2}')
    # Also capture with higher precision using user time
    local interp_user
    interp_user=$(echo "$t_interp" | grep user | awk '{print $2}')

    # Time VM
    local t_vm
    t_vm=$( { time $BINARY "$testfile" --vm >/dev/null 2>&1; } 2>&1 )
    local vm_real
    vm_real=$(echo "$t_vm" | grep real | awk '{print $2}')
    local vm_user
    vm_user=$(echo "$t_vm" | grep user | awk '{print $2}')

    printf "interp: %-12s  vm: %-12s" "$interp_real" "$vm_real"

    # Calculate speedup from user time (seconds)
    local i_sec v_sec
    i_sec=$(echo "$interp_user" | sed 's/[ms]/ /g' | awk '{printf "%.3f", $1*60+$2}')
    v_sec=$(echo "$vm_user" | sed 's/[ms]/ /g' | awk '{printf "%.3f", $1*60+$2}')

    if [ "$(echo "$v_sec > 0.001" | bc -l)" = "1" ]; then
        local speedup
        speedup=$(echo "scale=1; $i_sec / $v_sec" | bc -l)
        printf "  speedup: %sx" "$speedup"
    else
        printf "  speedup: (VM too fast to measure)"
    fi
    echo ""
}

echo "MMBasic Benchmark: Interpreter vs Bytecode VM"
echo "==============================================="
echo ""
printf "%-20s %-14s  %-14s  %s\n" "Benchmark" "Interpreter" "VM" "Speedup"
printf "%-20s %-14s  %-14s  %s\n" "---------" "-----------" "--" "-------"

if [ $# -ge 1 ]; then
    # Run specific benchmark
    file="bench_${1}.bas"
    if [ ! -f "$file" ]; then
        # Try without prefix
        file="${1}.bas"
    fi
    if [ ! -f "$file" ]; then
        echo "Benchmark not found: $1"
        echo "Available benchmarks:"
        ls bench_*.bas 2>/dev/null | sed 's/bench_//;s/\.bas//'
        exit 1
    fi
    run_bench "$file"
else
    # Run all benchmarks
    for f in bench_*.bas; do
        [ -f "$f" ] || continue
        run_bench "$f"
    done
fi

echo ""
echo "Note: Wall-clock times. Speedup based on user CPU time."
