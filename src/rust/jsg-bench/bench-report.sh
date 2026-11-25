#!/bin/bash
# Writes one JSON report per benchmark process to the test outputs; see README.md.
set -euo pipefail

for ((i = 1; i <= ${BENCH_PROCESSES:-3}; i++)); do
  "$1" --benchmark_dry_run=false \
    --benchmark_filter="${BENCH_FILTER:-.}" \
    --benchmark_repetitions=5 \
    --benchmark_enable_random_interleaving=true \
    --benchmark_out="$TEST_UNDECLARED_OUTPUTS_DIR/process-$i.json" \
    --benchmark_out_format=json >/dev/null
done
