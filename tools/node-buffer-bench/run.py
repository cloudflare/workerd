#!/usr/bin/env python3
# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

"""Compares the C++ and TypeScript node-internal:buffer implementations.

Runs the Node.js buffer benchmarks (benchmark/buffers in the Node.js repository)
under workerd, once with the NODEJS_BUFFER_TS autogate off (the C++ BufferUtil)
and once with it on (the TypeScript implementation), and reports the ratio of
their times.

The benchmarks are downloaded at a pinned Node.js commit. common.js in this
directory stands in for Node.js's benchmark/common.js. As with Node.js's
runner, each configuration runs in a fresh process: here, one `workerd test`
per configuration and implementation. The two implementations alternate which
runs first.

Inside workerd the clock has 3ms granularity, where Node.js has nanoseconds,
so many of the benchmarks' default operation counts (`n`) finish too quickly
to measure. Before measuring a configuration, its `n` is raised until the C++
implementation takes at least --min-time, and the same `n` is then used for
both implementations.

Build workerd with optimizations first:

    bazel build --config=benchmark --dynamic_mode=off //src/workerd/server:workerd

Examples:

    tools/node-buffer-bench/run.py
    tools/node-buffer-bench/run.py --filter 'indexof|compare' --runs 3
    tools/node-buffer-bench/run.py --filter buffer-swap --set len=64 --set len=1024
"""

import argparse
import csv
import json
import math
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import urllib.request
from collections import defaultdict
from pathlib import Path
from typing import NamedTuple

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
NODE_COMMIT = "3d85c94cfaddf661b5fe7a859d4ee08a08797924"
FIXTURES = ["alice.html"]
# Config autogate names carry the prefix that Autogate::initAutogate expects.
AUTOGATE = "workerd-autogate-nodejs-buffer-ts"
COMPAT_DATE = "2026-09-01"
IMPLS = ("cpp", "ts")

# Benchmarks that cannot run under workerd with either implementation.
UNSUPPORTED = {
    "buffer-copy-bytes-from.js": "workerd has no Buffer.copyBytesFrom",
    "buffer-normalize-encoding.js": "requires internal/util (--expose-internals)",
    "buffer-stringlength.js": "workerd has no Buffer.stringLength",
}


def fetch(url):
    with urllib.request.urlopen(url) as response:
        return response.read()


def download_benchmarks(commit, cache):
    """Returns the directory holding buffers/ and fixtures/ at `commit`."""
    root = cache / commit
    done = root / ".complete"
    if done.exists():
        return root
    listing = json.loads(
        fetch(
            f"https://api.github.com/repos/nodejs/node/contents/benchmark/buffers?ref={commit}"
        )
    )
    raw = f"https://raw.githubusercontent.com/nodejs/node/{commit}/benchmark"
    (root / "buffers").mkdir(parents=True, exist_ok=True)
    (root / "fixtures").mkdir(parents=True, exist_ok=True)
    for entry in listing:
        name = entry["name"]
        (root / "buffers" / name).write_bytes(fetch(f"{raw}/buffers/{name}"))
    for name in FIXTURES:
        (root / "fixtures" / name).write_bytes(fetch(f"{raw}/fixtures/{name}"))
    done.touch()
    return root


def capnp_string(value):
    return json.dumps(value)


class Job(NamedTuple):
    """One run of a benchmark script under workerd."""

    script: str
    # "list" returns the configuration queue; "run" runs configuration `index`.
    mode: str
    index: int
    # Configuration values overridden with --set: {key: [values]}.
    overrides: dict
    impl: str
    # The operation count, in place of the configuration's own, or None.
    n: int | None = None


def write_config(work, job):
    """Writes a workerd config for `job` and returns its path."""
    script, mode, index, overrides, impl, n = job
    # The entry module also reports which implementation is loaded: an
    # invalid hex fill throws from the TypeScript module's own code, so its
    # stack includes node-internal:buffer, and from native code otherwise.
    entry = (
        f"import 'buffers/{script}';\n"
        "import common from 'common.js';\n"
        "import { Buffer } from 'node:buffer';\n"
        "function implementation() {\n"
        "  try {\n"
        "    Buffer.alloc(2).fill('zz', 'hex');\n"
        "  } catch (error) {\n"
        "    return /node-internal:buffer\\b/.test(error.stack) ? 'ts' : 'cpp';\n"
        "  }\n"
        "  throw new Error('invalid hex fill did not throw');\n"
        "}\n"
        "export default {\n"
        "  async test(controller, env) {\n"
        "    const impl = implementation();\n"
        f"    const result = await common.run({json.dumps(mode)}, {index}, "
        f"{json.dumps(overrides)}, {json.dumps(n)}, env);\n"
        "    console.log('NODEBENCH ' + JSON.stringify({ impl, ...result }));\n"
        "  },\n"
        "};\n"
    )
    (work / "entry.js").write_text(entry)
    modules = [
        '(name = "entry.js", esModule = embed "entry.js")',
        '(name = "common.js", commonJsModule = embed "common.js")',
        # For require('../common'), which some benchmarks use.
        '(name = "common", commonJsModule = "module.exports = require(\'./common.js\');")',
        '(name = "fs.js", commonJsModule = embed "fs.js")',
        f"(name = {capnp_string('buffers/' + script)}, "
        f"commonJsModule = embed {capnp_string('buffers/' + script)})",
    ]
    for name in FIXTURES:
        path = f"fixtures/{name}"
        modules.append(
            f"(name = {capnp_string(path)}, data = embed {capnp_string(path)})"
        )
    autogates = f"[{capnp_string(AUTOGATE)}]" if impl == "ts" else "[]"
    config = f"""using Workerd = import "/workerd/workerd.capnp";
const config :Workerd.Config = (
  services = [(name = "bench", worker = (
    modules = [
      {("," + chr(10) + "      ").join(modules)}
    ],
    compatibilityDate = {capnp_string(COMPAT_DATE)},
    compatibilityFlags = ["nodejs_compat", "precise_timers"],
    bindings = [(name = "UNSAFE_EVAL", unsafeEval = void)],
  ))],
  autogates = {autogates},
);
"""
    path = work / "config.capnp"
    path.write_text(config)
    return path


def run_workerd(workerd, config, timeout, impl):
    """Runs `config` and returns the object its entry module printed."""
    process = subprocess.run(
        [str(workerd), "test", "--experimental", str(config)],
        capture_output=True,
        text=True,
        timeout=timeout,
    )
    output = process.stdout + process.stderr
    for line in output.splitlines():
        marker = line.find("NODEBENCH ")
        if marker != -1:
            result = json.loads(line[marker + len("NODEBENCH ") :])
            if result["impl"] != impl:
                raise RuntimeError(
                    f"expected the {impl} implementation, got {result['impl']}"
                )
            return result
    raise RuntimeError(output.strip()[-2000:])


def prepare(work, benchmarks, script):
    """Copies the files a run of `script` needs into `work`."""
    (work / "buffers").mkdir(exist_ok=True)
    (work / "fixtures").mkdir(exist_ok=True)
    shutil.copy(HERE / "common.js", work / "common.js")
    shutil.copy(HERE / "fs.js", work / "fs.js")
    for name in FIXTURES:
        shutil.copy(benchmarks / "fixtures" / name, work / "fixtures" / name)
    source = (benchmarks / "buffers" / script).read_text()
    source = re.sub(r"require\((['\"])(node:)?fs\1\)", "require('../fs.js')", source)
    source = source.replace("new Function(", "require('../common.js').newFunction(")
    (work / "buffers" / script).write_text(source)


def last_line(error):
    """The most informative line of a failed run's output."""
    lines = [line for line in str(error).splitlines() if line.strip()]
    for line in reversed(lines):
        if "stack = " in line:
            return line.split("stack = ", 1)[1][:200]
    return lines[-1][:200] if lines else ""


def calibrate(run, script, index, config, min_ms):
    """Returns an operation count for which configuration `index` of `script`
    takes at least `min_ms` with the C++ implementation."""
    n = config.get("n")
    if not isinstance(n, (int, float)):
        return None
    for _ in range(10):
        try:
            elapsed = run(script, index, "cpp", n)["elapsedMs"]
        except RuntimeError as error:
            if "insufficient clock precision" not in str(error):
                raise
            n *= 100
            continue
        if elapsed >= min_ms:
            return n
        n = math.ceil(n * 1.2 * min_ms / max(elapsed, 3))
    return n


def geomean(values):
    return math.exp(sum(math.log(v) for v in values) / len(values))


def summarize(rows, min_ms):
    """Prints TS / C++ time ratios from the median rate of each configuration."""
    rates = defaultdict(lambda: defaultdict(list))
    elapsed = defaultdict(list)
    for row in rows:
        key = (row["script"], row["config"])
        rates[key][row["impl"]].append(row["rate"])
        elapsed[key].append(row["elapsed_ms"])
    ratios = {}
    for key, byImpl in rates.items():
        if all(byImpl[impl] for impl in IMPLS):
            # Time ratio: the C++ rate over the TypeScript rate.
            ratios[key] = statistics.median(byImpl["cpp"]) / statistics.median(
                byImpl["ts"]
            )
    if not ratios:
        print("No results.")
        return

    print("\nTS / C++ time per operation (below 1.0 means TypeScript is faster)\n")
    byScript = defaultdict(list)
    for (script, _), ratio in ratios.items():
        byScript[script].append(ratio)
    print(f"{'benchmark':<36} {'configs':>7} {'geomean':>8} {'min':>6} {'max':>6}")
    for script in sorted(byScript):
        values = byScript[script]
        print(
            f"{script:<36} {len(values):>7} {geomean(values):>7.2f}x "
            f"{min(values):>5.2f}x {max(values):>5.2f}x"
        )
    values = list(ratios.values())
    print(
        f"\nAll {len(values)} configurations: geomean {geomean(values):.3f}x, "
        f"median {statistics.median(values):.2f}x, "
        f"{sum(v > 1.1 for v in values)} over 1.1x, "
        f"{sum(v < 0.9 for v in values)} under 0.9x"
    )
    print("\nSlowest configurations:")
    for (script, config), ratio in sorted(ratios.items(), key=lambda kv: -kv[1])[:15]:
        print(f"  {ratio:5.2f}x  {script} {config}")
    short = sum(1 for key in ratios if min(elapsed[key]) < min_ms * 0.9)
    if short:
        print(
            f"\n{short} configurations had a run well under --min-time; treat "
            "their ratios as less precise."
        )


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--workerd",
        type=Path,
        default=ROOT / "bazel-bin/src/workerd/server/workerd",
        help="workerd binary (default: %(default)s)",
    )
    parser.add_argument(
        "--node-commit",
        default=NODE_COMMIT,
        help="Node.js commit to take the benchmarks from (default: %(default)s)",
    )
    parser.add_argument(
        "--cache",
        type=Path,
        default=Path.home() / ".cache/workerd-node-buffer-bench",
        help="where downloaded benchmarks are kept (default: %(default)s)",
    )
    parser.add_argument(
        "--filter", default="", help="only run benchmarks whose file name matches"
    )
    parser.add_argument(
        "--set",
        action="append",
        default=[],
        metavar="KEY=VALUE",
        help="override a configuration value, as on Node.js's command line; repeat "
        "a key for several values",
    )
    parser.add_argument(
        "--min-time",
        type=float,
        default=1.0,
        help="seconds each run should take at least, by raising n (default: "
        "%(default)s)",
    )
    parser.add_argument(
        "--runs", type=int, default=1, help="runs of each configuration (default: 1)"
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=600,
        help="seconds allowed per run (default: %(default)s)",
    )
    parser.add_argument(
        "--out", type=Path, help="write every run to this CSV file as it completes"
    )
    args = parser.parse_args()

    if not args.workerd.exists():
        sys.exit(f"{args.workerd} not found; build workerd first (see --help)")
    overrides = defaultdict(list)
    for item in args.set:
        key, _, value = item.partition("=")
        overrides[key].append(value)

    benchmarks = download_benchmarks(args.node_commit, args.cache)
    scripts = sorted(
        path.name
        for path in (benchmarks / "buffers").glob("*.js")
        if not path.name.startswith("_") and re.search(args.filter, path.name)
    )

    out = out_file = None
    if args.out:
        out_file = args.out.open("w", newline="")
        out = csv.writer(out_file)
        out.writerow(["script", "config", "n", "impl", "run", "rate", "elapsed_ms"])
    rows = []
    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)

        def run(script, index, impl, n):
            return run_workerd(
                args.workerd,
                write_config(work, Job(script, "run", index, overrides, impl, n)),
                args.timeout,
                impl,
            )

        for script in scripts:
            if script in UNSUPPORTED:
                print(f"{script}: skipped, {UNSUPPORTED[script]}")
                continue
            prepare(work, benchmarks, script)
            try:
                queue = run_workerd(
                    args.workerd,
                    write_config(work, Job(script, "list", 0, overrides, "cpp")),
                    args.timeout,
                    "cpp",
                )["queue"]
            except Exception as error:
                failures.append((script, "(loading)", str(error)))
                print(f"{script}: failed to load: {last_line(error)}")
                continue
            for index, config in enumerate(queue):
                label = json.dumps(config, sort_keys=True)
                try:
                    n = calibrate(run, script, index, config, args.min_time * 1000)
                except Exception as error:
                    failures.append((script, label, str(error)))
                    print(f"{script} {label}: failed: {last_line(error)}")
                    continue
                for run_index in range(args.runs):
                    order = IMPLS if (index + run_index) % 2 == 0 else IMPLS[::-1]
                    result = {}
                    for impl in order:
                        try:
                            result[impl] = run(script, index, impl, n)
                        except Exception as error:
                            failures.append((script, label, f"{impl}: {error}"))
                            print(
                                f"{script} {label} {impl}: failed: {last_line(error)}"
                            )
                            continue
                        row = {
                            "script": script,
                            "config": label,
                            "n": n,
                            "impl": impl,
                            "run": run_index,
                            "rate": result[impl]["rate"],
                            "elapsed_ms": result[impl]["elapsedMs"],
                        }
                        rows.append(row)
                        if out:
                            out.writerow(row.values())
                            out_file.flush()
                    if len(result) == 2:
                        ratio = result["cpp"]["rate"] / result["ts"]["rate"]
                        print(f"{ratio:5.2f}x  {script} {label} n={n}", flush=True)

    summarize(rows, args.min_time * 1000)
    if failures:
        print(f"\n{len(failures)} configurations or runs failed:")
        byError = defaultdict(list)
        for script, label, error in failures:
            byError[(script, last_line(error))].append(label)
        for (script, error), labels in sorted(byError.items()):
            print(f"  {script} ({len(labels)}): {error}")
        sys.exit(1)


if __name__ == "__main__":
    main()
