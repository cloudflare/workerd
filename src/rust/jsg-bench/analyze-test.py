#!/usr/bin/env python3
"""Synthetic regression checks for batch normalization and comparison coverage."""

import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


def load(name):
    spec = importlib.util.spec_from_file_location(
        name, Path(__file__).with_name(name + ".py")
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


runner = load("run")
smoke = load("smoke-test")


class AnalyzerTest(unittest.TestCase):
    def write_process(self, root, process, measurements):
        rows = []
        for name, time, unit in measurements:
            rows.extend(
                {
                    "run_type": "iteration",
                    "run_name": name,
                    "time_unit": unit,
                    "cpu_time": time,
                    "real_time": time * 2,
                    "ops/batch": 10,
                }
                for _ in range(5)
            )
        (root / f"process-{process}.json").write_text(json.dumps({"benchmarks": rows}))

    def run_analyzer(self, directory):
        return subprocess.run(
            [sys.executable, str(Path(__file__).with_name("analyze.py")), directory],
            capture_output=True,
            text=True,
            check=False,
        )

    def test_exact_ratios_and_batch_units(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for process in range(3):
                self.write_process(
                    root,
                    process,
                    [
                        ("Cpp/example", 100, "ns"),
                        ("Rust/example", 0.2, "us"),
                        ("CppFast/example", 50, "ns"),
                    ],
                )
            result = self.run_analyzer(directory)
            self.assertEqual(result.returncode, 0, result.stderr)
            summary = json.loads((root / "summary.json").read_text())
            self.assertEqual(len(summary), 2)
            for index, row in enumerate(summary):
                cpp = [10, 5][index]
                ratio = [2, 4][index]
                cpu = row["cpu_time"]
                self.assertEqual(cpu["cpp"], cpp)
                self.assertEqual(cpu["rust"], 20)
                self.assertEqual(cpu["ratio"], ratio)
                self.assertEqual(cpu["ratio_min"], ratio)
                self.assertEqual(cpu["ratio_max"], ratio)
                self.assertEqual(row["real_time"]["rust"], 40)
            self.assertIn("elapsed ns/op", result.stdout)

    def test_ratios_pair_measurements_within_each_process(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for process, (cpp, rust) in enumerate(
                [(10, 20), (1000, 1000), (10000, 40000)]
            ):
                self.write_process(
                    root,
                    process,
                    [("Cpp/example", cpp, "ns"), ("Rust/example", rust, "ns")],
                )
            result = self.run_analyzer(directory)
            self.assertEqual(result.returncode, 0, result.stderr)
            cpu = json.loads((root / "summary.json").read_text())[0]["cpu_time"]
            self.assertEqual(cpu["ratio"], 2)
            self.assertEqual(cpu["process_ratios"], [2, 1, 4])
            self.assertEqual((cpu["ratio_min"], cpu["ratio_max"]), (1, 4))

    def test_missing_counterpart_has_a_clear_error(self):
        with tempfile.TemporaryDirectory() as directory:
            self.write_process(Path(directory), 0, [("Cpp/example", 1, "ns")])
            result = self.run_analyzer(directory)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("no Rust counterpart", result.stderr)
            self.assertNotIn("Traceback", result.stderr)

    def test_different_processes_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            self.write_process(Path(directory), 0, [("Cpp/example", 1, "ns")])
            self.write_process(Path(directory), 1, [("Rust/example", 1, "ns")])
            result = self.run_analyzer(directory)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("different processes", result.stderr)

    def test_empty_directory_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertNotEqual(self.run_analyzer(directory).returncode, 0)

    def test_failed_measurement_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            (Path(directory) / "process-1.json").write_text(
                json.dumps(
                    {"benchmarks": [{"run_type": "iteration", "error_occurred": True}]}
                )
            )
            self.assertNotEqual(self.run_analyzer(directory).returncode, 0)


class RunnerTest(unittest.TestCase):
    def test_linux_diagnostics(self):
        with (
            mock.patch.object(runner.platform, "system", return_value="Linux"),
            mock.patch.object(runner, "capture", side_effect=lambda command: command),
        ):
            commands = runner.machine_diagnostics()
        for command in [
            ["lscpu"],
            ["uname", "-r"],
            ["nproc"],
            ["ps", "-eo", "pid,pcpu,comm", "--sort=-pcpu"],
        ]:
            self.assertIn(command, commands)
        self.assertFalse(
            any(c[0] in ("sysctl", "sw_vers", "xcrun", "pmset") for c in commands)
        )

    def test_git_is_only_used_outside_jj_repositories(self):
        for root_status, tool in [(0, "jj"), (1, "git")]:
            with (
                self.subTest(root_status=root_status),
                mock.patch.object(
                    runner, "capture", return_value={"exit_code": root_status}
                ) as capture,
            ):
                runner.version_control_diagnostics()
                self.assertEqual(
                    [call.args[0][0] for call in capture.call_args_list],
                    ["jj", tool, tool],
                )

    def test_linux_cpu_governors(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "policy0"
            path.mkdir()
            (path / "scaling_governor").write_text("performance\n")
            self.assertEqual(
                runner.cpu_governors(Path(directory)), {"policy0": "performance"}
            )

    def test_missing_binary_does_not_create_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            result = subprocess.run(
                [
                    sys.executable,
                    str(Path(__file__).with_name("run.py")),
                    "--binary",
                    str(root / "missing"),
                    "--output",
                    str(root / "out"),
                ],
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((root / "out").exists())

    def test_instrumentation_is_rejected(self):
        result = subprocess.run(
            [
                sys.executable,
                str(Path(__file__).with_name("run.py")),
                "--output",
                "unused",
            ],
            env={**os.environ, "JSG_BENCH_VERIFY_FAST": "1"},
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unset JSG_BENCH_VERIFY_FAST", result.stderr)


class SmokeTest(unittest.TestCase):
    def report(self, **fields):
        return {
            "benchmarks": [
                {
                    "run_type": "iteration",
                    "run_name": "CppFast/dispatch/number",
                    "fast-calls": 1024,
                    "slow-calls": 0,
                    **fields,
                }
            ]
        }

    def test_fast_calls_pass(self):
        smoke.validate(self.report())

    def test_benchmark_errors_fail_even_with_fast_calls(self):
        with self.assertRaisesRegex(RuntimeError, "benchmark failures"):
            smoke.validate(
                self.report(error_occurred=True, error_message="batch result")
            )

    def test_missing_or_slow_fast_api_calls_fail(self):
        for fields in [{"fast-calls": 0}, {"slow-calls": 1}]:
            with (
                self.subTest(fields=fields),
                self.assertRaisesRegex(RuntimeError, "Fast API verification failed"),
            ):
                smoke.validate(self.report(**fields))

    def test_empty_run_fails(self):
        with self.assertRaisesRegex(RuntimeError, "no benchmarks ran"):
            smoke.validate({"benchmarks": []})


if __name__ == "__main__":
    unittest.main()
