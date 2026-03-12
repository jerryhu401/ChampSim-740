#!/usr/bin/env bash
set -euo pipefail

pushd "$PWD/$(dirname "$0")/.." > /dev/null

WARMUP=${WARMUP:-5000000}
SIM=${SIM:-10000000}
TRACE=${TRACE:-traces/600.perlbench_s-210B.champsimtrace.xz}

RESULTS_DIR="scripts/results"
mkdir -p "$RESULTS_DIR"

if [ ! -f "$TRACE" ]; then
    echo "ERROR: trace not found at $TRACE"
    echo "Download one with:"
    echo "  wget -P traces/ https://dpc3.compas.cs.stonybrook.edu/champsim-traces/speccpu/600.perlbench_s-210B.champsimtrace.xz"
    exit 1
fi

echo "=== Config ==="
echo "  Warmup:     $WARMUP instructions"
echo "  Simulation: $SIM instructions"
echo "  Trace:      $TRACE"
echo ""

echo "=== [1/3] Running baseline (no prefetcher) ==="
bin/champsim \
    --warmup-instructions "$WARMUP" \
    --simulation-instructions "$SIM" \
    "$TRACE" 2>&1 | tee "$RESULTS_DIR/baseline.txt"

echo ""
echo "=== [2/3] Running next_line L2C prefetcher ==="
bin/champsim_next_line \
    --warmup-instructions "$WARMUP" \
    --simulation-instructions "$SIM" \
    "$TRACE" 2>&1 | tee "$RESULTS_DIR/next_line.txt"

echo ""
echo "=== [3/3] Running IMP L2C prefetcher ==="
bin/champsim_imp \
    --warmup-instructions "$WARMUP" \
    --simulation-instructions "$SIM" \
    "$TRACE" 2>&1 | tee "$RESULTS_DIR/imp.txt"

echo ""
echo "================================================================"
echo "  COMPARISON SUMMARY"
echo "================================================================"

baseline_ipc=$(grep "CPU 0 cumulative IPC" "$RESULTS_DIR/baseline.txt" | tail -1 | awk '{print $5}')
nextline_ipc=$(grep "CPU 0 cumulative IPC" "$RESULTS_DIR/next_line.txt" | tail -1 | awk '{print $5}')
imp_ipc=$(grep "CPU 0 cumulative IPC" "$RESULTS_DIR/imp.txt" | tail -1 | awk '{print $5}')

echo ""
echo "  IPC (baseline):    $baseline_ipc"
echo "  IPC (next_line):   $nextline_ipc"
echo "  IPC (IMP):         $imp_ipc"
nl_speedup=$(python3 -c "print(f'{($nextline_ipc / $baseline_ipc - 1) * 100:+.2f}%')")
imp_speedup=$(python3 -c "print(f'{($imp_ipc / $baseline_ipc - 1) * 100:+.2f}%')")
echo "  Speedup next_line: $nl_speedup"
echo "  Speedup IMP:       $imp_speedup"

echo ""
echo "--- L2C Prefetch Stats (next_line) ---"
grep -A1 "cpu0->cpu0_L2C PREFETCH" "$RESULTS_DIR/next_line.txt"

echo ""
echo "--- L2C Prefetch Stats (IMP) ---"
grep -A1 "cpu0->cpu0_L2C PREFETCH" "$RESULTS_DIR/imp.txt"

echo ""
echo "--- IMP Custom Stats ---"
grep "^\[IMP\]" "$RESULTS_DIR/imp.txt" || true

echo ""
echo "--- LLC Stats ---"
echo "  Baseline:"
grep "cpu0->LLC TOTAL" "$RESULTS_DIR/baseline.txt"
echo "  next_line:"
grep "cpu0->LLC TOTAL" "$RESULTS_DIR/next_line.txt"
echo "  IMP:"
grep "cpu0->LLC TOTAL" "$RESULTS_DIR/imp.txt"

echo ""
echo "Full results saved to:"
echo "  $RESULTS_DIR/baseline.txt"
echo "  $RESULTS_DIR/next_line.txt"
echo "  $RESULTS_DIR/imp.txt"
