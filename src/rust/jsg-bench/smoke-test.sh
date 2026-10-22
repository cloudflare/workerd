#!/bin/bash
# Runs every benchmark case once. In verification mode, a wrong result or a missed Fast API
# callback aborts the benchmark executable, which fails this test.
set -euo pipefail

unset JSG_BENCH_NATURAL_JIT
JSG_BENCH_VERIFY=1 exec "$1" --benchmark_dry_run=true
