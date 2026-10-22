#!/usr/bin/env python3
"""Check benchmark results and Fast API entry without collecting performance data."""

import json
import os
import subprocess
import sys
from pathlib import Path


def validate(report):
    rows = [r for r in report["benchmarks"] if r.get("run_type") == "iteration"]
    if not rows:
        raise RuntimeError("no benchmarks ran")
    failures = [r for r in rows if r.get("error_occurred")]
    if failures:
        raise RuntimeError(f"benchmark failures: {failures}")
    fast = [r for r in rows if r["run_name"].startswith("CppFast/")]
    if not fast:
        raise RuntimeError("no Fast API cases ran")
    for row in fast:
        if row.get("fast-calls", 0) == 0 or row.get("slow-calls", 0) != 0:
            raise RuntimeError(f"Fast API verification failed: {row}")


def main():
    output = Path(os.environ["TEST_TMPDIR"]) / "benchmarks.json"
    env = {**os.environ, "JSG_BENCH_VERIFY_FAST": "1"}
    env.pop("JSG_BENCH_NATURAL_JIT", None)
    subprocess.run(
        [
            str(Path(sys.argv[1]).resolve()),
            "--benchmark_dry_run=true",
            f"--benchmark_out={output}",
            "--benchmark_out_format=json",
        ],
        env=env,
        check=True,
    )
    validate(json.loads(output.read_text()))


if __name__ == "__main__":
    main()
