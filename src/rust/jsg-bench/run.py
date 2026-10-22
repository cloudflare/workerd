#!/usr/bin/env python3
"""Run independent, randomized JSG benchmark processes and retain every sample."""

import argparse
import datetime
import hashlib
import json
import os
import platform
import subprocess
from pathlib import Path

UTC = datetime.timezone.utc  # noqa: UP017 - support the macOS system Python 3.9


def capture(command):
    try:
        result = subprocess.run(command, text=True, capture_output=True, check=False)
    except OSError as error:
        return {"command": command, "exit_code": None, "error": str(error)}
    return {
        "command": command,
        "exit_code": result.returncode,
        "stdout": result.stdout,
        "stderr": result.stderr,
    }


def version_control_diagnostics():
    if capture(["jj", "root"]).get("exit_code") == 0:
        commands = [
            ["jj", "log", "-r", "@", "--no-graph", "-T", "commit_id"],
            ["jj", "diff", "--stat"],
        ]
    else:
        commands = [["git", "rev-parse", "HEAD"], ["git", "diff", "--stat"]]
    return [capture(command) for command in commands]


def machine_diagnostics():
    commands = [["bazel", "--version"]]
    if platform.system() == "Darwin":
        commands += [
            ["sysctl", "hw.model", "hw.memsize", "hw.ncpu"],
            ["sw_vers"],
            ["xcrun", "clang", "--version"],
            ["pmset", "-g", "batt"],
            ["pmset", "-g", "therm"],
            ["ps", "-Ao", "pid,pcpu,comm", "-r"],
        ]
    elif platform.system() == "Linux":
        commands += [
            ["lscpu"],
            ["uname", "-r"],
            ["nproc"],
            ["ps", "-eo", "pid,pcpu,comm", "--sort=-pcpu"],
        ]
    return [capture(command) for command in commands]


def cpu_governors(root=Path("/sys/devices/system/cpu/cpufreq")):
    governors = {}
    for path in sorted(root.glob("policy*/scaling_governor")):
        try:
            governors[path.parent.name] = path.read_text().strip()
        except OSError as error:
            governors[path.parent.name] = str(error)
    return governors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--binary", type=Path, default=Path("bazel-bin/src/rust/jsg-bench/bench-jsg")
    )
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--processes", type=int, default=3)
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--min-time", type=float, default=0.12)
    parser.add_argument("--filter", default=".")
    parser.add_argument("--natural-jit", action="store_true")
    args = parser.parse_args()
    if args.processes < 1 or args.repetitions < 1 or args.min_time <= 0:
        parser.error("processes, repetitions, and min-time must be positive")
    if "JSG_BENCH_VERIFY_FAST" in os.environ:
        parser.error(
            "unset JSG_BENCH_VERIFY_FAST: instrumented runs are verification only"
        )
    binary = args.binary.resolve()
    if not binary.is_file():
        parser.error(
            f"benchmark binary not found: {binary}; rebuild with --config=benchmark "
            "or pass --binary"
        )
    names = subprocess.check_output(
        [
            str(binary),
            "--benchmark_list_tests=true",
            f"--benchmark_filter={args.filter}",
        ],
        text=True,
    ).splitlines()
    if not names:
        raise RuntimeError("benchmark filter matched no cases")
    args.output.mkdir(parents=True, exist_ok=False)
    metadata = {
        "started": datetime.datetime.now(UTC).isoformat(),
        "binary": str(binary),
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "platform": platform.platform(),
        "cases": names,
        "arguments": vars(args).copy(),
        "environment": {
            k: v
            for k, v in os.environ.items()
            if k.startswith(("JSG_BENCH_", "Malloc", "TCMALLOC_"))
        },
        "diagnostics": version_control_diagnostics() + machine_diagnostics(),
        "cpu_governors": cpu_governors() if platform.system() == "Linux" else {},
    }
    metadata["arguments"] = {
        k: str(v) if isinstance(v, Path) else v
        for k, v in metadata["arguments"].items()
    }
    (args.output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    env = os.environ.copy()
    if args.natural_jit:
        env["JSG_BENCH_NATURAL_JIT"] = "1"
    for index in range(args.processes):
        output = (args.output / f"process-{index + 1}.json").resolve()
        command = [
            str(binary),
            f"--benchmark_filter={args.filter}",
            f"--benchmark_min_time={args.min_time}s",
            f"--benchmark_repetitions={args.repetitions}",
            "--benchmark_enable_random_interleaving=true",
            f"--benchmark_out={output}",
            "--benchmark_out_format=json",
            "--benchmark_color=false",
        ]
        print(f"Process {index + 1}/{args.processes}: {len(names)} cases", flush=True)
        with (args.output / f"process-{index + 1}.log").open("w") as log:
            subprocess.run(
                command, stdout=log, stderr=subprocess.STDOUT, check=True, env=env
            )
        report = json.loads(output.read_text())
        rows = [
            row for row in report["benchmarks"] if row.get("run_type") == "iteration"
        ]
        errors = [row for row in rows if row.get("error_occurred")]
        observed = {row["run_name"] for row in rows}
        if (
            errors
            or observed != set(names)
            or len(rows) != len(names) * args.repetitions
        ):
            raise RuntimeError(
                f"missing or failed benchmarks: {errors}; "
                f"expected {len(names) * args.repetitions}, got {len(rows)}"
            )
        print(f"Validated {len(rows)} fresh measurements", flush=True)
    print(f"Results: {args.output}", flush=True)
    (args.output / "finished.json").write_text(
        json.dumps(
            {
                "finished": datetime.datetime.now(UTC).isoformat(),
                "diagnostics": machine_diagnostics(),
                "cpu_governors": cpu_governors()
                if platform.system() == "Linux"
                else {},
            },
            indent=2,
        )
        + "\n"
    )


if __name__ == "__main__":
    main()
