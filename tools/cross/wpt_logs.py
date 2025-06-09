#!/usr/bin/env python3
import argparse
import json
import os
import re
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Optional

TEST_TARGET = "//src/wpt/..."
LOGS_DIR = Path("bazel-testlogs/src/wpt")


@dataclass
class Options:
    config: bool
    report: bool
    stats: bool


def main() -> None:
    cmd = argparse.ArgumentParser()
    cmd.add_argument("--config", action="store_true")
    cmd.add_argument("--report", type=Path)
    cmd.add_argument("--stats", type=Path)
    args = cmd.parse_args()
    options = Options(args.config, bool(args.report), bool(args.stats))
    time_interval = run_tests(TEST_TARGET, options)
    logs = parse_logs(LOGS_DIR, options)

    if args.stats:
        args.stats.write_text(stats_table(logs))

    if args.report:
        report = wpt_report(logs, time_interval)
        with args.report.open("w") as fp:
            json.dump(report, fp, indent=2)

    # TODO(soon): Implement the config option to update test configs


@dataclass
class TimeInterval:
    start: int = 0
    end: int = 0


def run_tests(test_target: str, options: Options) -> TimeInterval:
    extra_args = os.environ.get("BAZEL_ARGS").split(" ")
    cmd = ["bazel", "test", *extra_args, test_target]

    if options.config:
        cmd.append("--test_env=GEN_TEST_CONFIG=1")

    if options.report:
        cmd.append("--test_env=GEN_TEST_REPORT=1")

    if options.stats:
        cmd.append("--test_env=GEN_TEST_STATS=1")

    interval = TimeInterval(int(time.time()))
    subprocess.run(cmd)
    interval.end = int(time.time())
    return interval


@dataclass
class Log:
    # TypeScript test config used to configure WPT tests
    config: Optional[str] = None

    # JSON report in WPT's format
    report: Optional[dict[str, Any]] = None

    # Markdown table row for human-readable stats
    stats: Optional[str] = None


def parse_logs(logs_dir: Path, options: Options) -> list[Log]:
    return [parse_log(log_file, options) for log_file in logs_dir.glob("*/test.log")]


def parse_log(log_file: Path, options: Options) -> list[Log]:
    log = Log()
    text = log_file.read_text()

    start_log = re.search(r"\[ TEST \] .*:zzz_results\n", text)
    if not start_log:
        return log

    end_log = re.search(r"^.*\[ (PASS|FAIL) \].*:zzz_results", text, re.MULTILINE)

    if not end_log:
        return log

    results_log = text[start_log.end(0) : end_log.start(0)]
    items = results_log.split("***")

    if options.config:
        # Removes a message for the user that we don't need
        items.pop(0)
        items.pop(0)
        log.config = items.pop(0).strip()

    if options.report:
        log.report = json.loads(items.pop(0))

    if options.stats:
        log.stats = items.pop(0).strip()

    return log


def stats_table(logs: list[Log]) -> str:
    table = """
| Module   | Coverage (ok/disabled/total/% ok) | Pass (pass/fail/disabled/total/% pass) |
|----------|-----------------------------------|----------------------------------------|
"""

    for log in logs:
        if log.stats:
            table += log.stats + "\n"

    return table


def cmd_output(cmd: list[str]) -> str:
    return subprocess.run(cmd, capture_output=True, check=True).stdout.decode().strip()


def get_os() -> str:
    """
    Return one of three expected values
    <https://github.com/web-platform-tests/wpt/blob/1c6ff12/tools/wptrunner/wptrunner/tests/test_update.py#L953-L958>
    """

    if sys.platform == "darwin":
        return "mac"
    elif sys.platform == "linux":
        return "linux"
    elif sys.platform == "win32":
        return "win"
    else:
        raise ValueError("Unsupported os type")


def wpt_report(logs: list[Log], time_interval: TimeInterval) -> dict[str, Any]:
    return {
        "time_start": time_interval.start,
        "time_end": time_interval.end,
        "run_info": {
            "product": "workerd",
            "browser_channel": "experimental",
            "browser_version": cmd_output(["git", "describe", "--tags"]),
            "revision": cmd_output(["git", "rev-parse", "HEAD"]),
            "os": get_os(),
        },
        "results": [
            result for log in logs if log.report for result in log.report["results"]
        ],
    }


if __name__ == "__main__":
    main()
