#!/usr/bin/env python3
"""Report per-operation times and paired Rust/C++ ratios for each process."""

import argparse
import csv
import json
import statistics
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    processes = sorted(args.directory.glob("process-*.json"))
    if not processes:
        parser.error("no process result files found")
    data = {}
    for path in processes:
        for row in json.loads(path.read_text())["benchmarks"]:
            if row.get("run_type") != "iteration":
                continue
            if row.get("error_occurred"):
                raise RuntimeError(row)
            scale = {"ns": 1, "us": 1000, "ms": 1000000, "s": 1000000000}[
                row["time_unit"]
            ]
            ops = row["ops/batch"]
            value = {k: row[k] * scale / ops for k in ("cpu_time", "real_time")}
            data.setdefault(row["run_name"], {}).setdefault(path.name, []).append(value)
    rows = []
    for reference in sorted(
        n for n in data if n.startswith(("Cpp/", "CppFast/", "CppStructFast/"))
    ):
        binding, name = reference.split("/", 1)
        cpp = data[reference]
        counterpart = "Rust/" + name
        if counterpart not in data:
            parser.error(f"{reference} has no Rust counterpart ({counterpart})")
        rust = data[counterpart]
        if set(cpp) != set(rust):
            parser.error(
                f"{reference} and {counterpart} have measurements from different processes"
            )
        shared = sorted(cpp)
        medians = {}
        for metric in ("cpu_time", "real_time"):
            c = [statistics.median(v[metric] for v in cpp[p]) for p in shared]
            r = [statistics.median(v[metric] for v in rust[p]) for p in shared]
            ratios = [r[i] / c[i] for i in range(len(shared))]
            medians[metric] = {
                "cpp": statistics.median(c),
                "rust": statistics.median(r),
                "ratio": statistics.median(ratios),
                "ratio_min": min(ratios),
                "ratio_max": max(ratios),
                "process_ratios": ratios,
                "cpp_process_medians": c,
                "rust_process_medians": r,
            }
        rows.append({"case": name, "reference": binding, **medians})
    if not rows:
        parser.error("no paired C++ and Rust measurements found")
    (args.directory / "summary.json").write_text(json.dumps(rows, indent=2) + "\n")
    with (args.directory / "summary.csv").open("w") as file:
        writer = csv.writer(file)
        writer.writerow(
            [
                "case",
                "reference",
                "cpp_cpu_ns",
                "rust_cpu_ns",
                "cpu_ratio",
                "cpu_ratio_min",
                "cpu_ratio_max",
                "cpp_wall_ns",
                "rust_wall_ns",
                "wall_ratio",
                "wall_ratio_min",
                "wall_ratio_max",
            ]
        )
        for row in rows:
            c, w = row["cpu_time"], row["real_time"]
            writer.writerow(
                [
                    row["case"],
                    row["reference"],
                    c["cpp"],
                    c["rust"],
                    c["ratio"],
                    c["ratio_min"],
                    c["ratio_max"],
                    w["cpp"],
                    w["rust"],
                    w["ratio"],
                    w["ratio_min"],
                    w["ratio_max"],
                ]
            )
    print(
        "| Case | Reference | C++ CPU ns/op | Rust CPU ns/op | Rust/C++ | Process ratio range | C++ elapsed ns/op | Rust elapsed ns/op |"
    )
    print("|---|---|---:|---:|---:|---:|---:|---:|")
    for row in rows:
        c, w = row["cpu_time"], row["real_time"]
        print(
            f"| {row['case']} | {row['reference']} | {c['cpp']:.2f} | {c['rust']:.2f} | {c['ratio']:.2f}x | "
            f"{c['ratio_min']:.2f}-{c['ratio_max']:.2f} | {w['cpp']:.2f} | {w['rust']:.2f} |"
        )
    print(f"\n{len(rows)} comparisons, {len(processes)} independent processes.")


if __name__ == "__main__":
    main()
