#!/bin/bash
# Warm up, then run bench RUNS times (default 3) for the given sizes.
# Usage: bench/run_median.sh OUTDIR [sizes...]   (run from the repo root,
# after cmake --build build; the CPU is pinned with taskset when possible)
set -u
OUT=${1:?outdir}; shift
SIZES=${*:-"1000000 10000000 100000000"}
RUNS=${RUNS:-3}
CPU=${CPU:-2}
mkdir -p "$OUT"
g++ -O2 -std=c++17 bench/warmup.cpp -o "$OUT/warmup"
PIN=""; command -v taskset >/dev/null && PIN="taskset -c $CPU"
echo "$(date -Is) warm-up" | tee -a "$OUT/progress.log"
$PIN "$OUT/warmup" 12 5 | tee "$OUT/warmup.log"
for r in $(seq 1 "$RUNS"); do
  echo "$(date -Is) run $r start" | tee -a "$OUT/progress.log"
  $PIN ./build/bench $SIZES > "$OUT/run$r.log" 2>&1
  echo "$(date -Is) run $r done exit $?" | tee -a "$OUT/progress.log"
done
echo "$(date -Is) ALL DONE" | tee -a "$OUT/progress.log"
