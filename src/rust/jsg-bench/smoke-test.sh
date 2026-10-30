#!/bin/bash
# Runs small versions of every benchmark case once. A wrong result or a missed Fast API
# callback aborts the benchmark executable, which fails this test.
set -euo pipefail

unset JSG_BENCH_NATURAL_JIT
JSG_BENCH_SMOKE=1 JSG_BENCH_VERIFY=1 exec "$1" --benchmark_dry_run=true
