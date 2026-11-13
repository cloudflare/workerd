#!/bin/bash
# Runs every benchmark case once. A fixture that fails to decode, or decodes to the wrong
# values, aborts the benchmark executable, which fails this test.
set -euo pipefail

R2_JSON_BENCH_SMOKE=1 exec "$1" --benchmark_dry_run=true
