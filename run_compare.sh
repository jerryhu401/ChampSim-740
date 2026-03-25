#!/bin/bash
# Compare prefetcher configs across traces
# Usage: ./run_compare.sh

WARMUP=1000000
SIM=10000000
TRACES=(traces/*.champsimtrace.xz)
BINARIES=(bin/champsim bin/champsim_bop bin/champsim_ipcp bin/champsim_berti)
RESULTS_DIR=results
mkdir -p "$RESULTS_DIR"

echo "=== Prefetcher Comparison ==="
echo "Warmup: ${WARMUP} | Sim: ${SIM}"
echo ""

# Run all combinations
for trace in "${TRACES[@]}"; do
  tname=$(basename "$trace" .champsimtrace.xz)
  for bin in "${BINARIES[@]}"; do
    bname=$(basename "$bin")
    outfile="${RESULTS_DIR}/${bname}_${tname}.txt"
    if [ ! -f "$outfile" ]; then
      echo "Running $bname on $tname..."
      ./"$bin" --warmup-instructions "$WARMUP" --simulation-instructions "$SIM" "$trace" > "$outfile" 2>&1
    else
      echo "Skipping $bname on $tname (already exists)"
    fi
  done
done

# Parse and compare
echo ""
printf "%-30s %-15s %-10s %-10s %-10s %-12s\n" "Trace" "Prefetcher" "IPC" "PF_Useful" "PF_Useless" "L2C_Miss"
printf "%-30s %-15s %-10s %-10s %-10s %-12s\n" "-----" "----------" "---" "---------" "----------" "--------"

for trace in "${TRACES[@]}"; do
  tname=$(basename "$trace" .champsimtrace.xz)
  for bin in "${BINARIES[@]}"; do
    bname=$(basename "$bin")
    outfile="${RESULTS_DIR}/${bname}_${tname}.txt"
    if [ -f "$outfile" ]; then
      ipc=$(grep "CPU 0 cumulative IPC:" "$outfile" | head -1 | awk '{print $5}')
      l2_useful=$(grep "cpu0->cpu0_L2C PREFETCH REQUESTED" "$outfile" | awk '{print $8}')
      l2_useless=$(grep "cpu0->cpu0_L2C PREFETCH REQUESTED" "$outfile" | awk '{print $10}')
      l2_miss=$(grep "cpu0->cpu0_L2C TOTAL" "$outfile" | awk '{print $8}')
      printf "%-30s %-15s %-10s %-10s %-10s %-12s\n" "$tname" "$bname" "$ipc" "$l2_useful" "$l2_useless" "$l2_miss"
    fi
  done
done
