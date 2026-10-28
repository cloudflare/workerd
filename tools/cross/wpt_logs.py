#!/usr/bin/env python3
import argparse
import json
import re
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Optional

EXCLUDED_TESTS = {
    # The WPT compliant implementation is in urlpattern-standard@
    "urlpattern@"
}


@dataclass
class Options:
    config: bool
    report: bool
    stats: bool


def main() -> None:
    cmd = argparse.ArgumentParser()
    cmd.add_argument("--update-config", action="store_true")
    cmd.add_argument("--print-stats", action="store_true")
    cmd.add_argument("logs_dir", type=Path)

    args = cmd.parse_args()
    # The logs that CI keeps always hold the report section, which comes
    # before the stats.
    options = Options(args.update_config, True, args.print_stats)
    logs = parse_logs(args.logs_dir, options)

    if args.print_stats:
        print(stats_table(logs))

    # TODO(soon): Implement the config option to update test configs


@dataclass
class TimeInterval:
    start: int = 0
    end: int = 0


@dataclass
class Log:
    # The suite's name, its test target's path under src/wpt, such as "fetch/api-ts@"
    suite: str = ""

    # TypeScript test config used to configure WPT tests
    config: Optional[str] = None

    # JSON report in WPT's format
    report: Optional[dict[str, Any]] = None

    # Summary stats in JSON
    stats: Optional[list[Any]] = None


def parse_logs(logs_dir: Path, options: Options) -> list[Log]:
    return [
        parse_log(log_file, options)
        for log_file in sorted(logs_dir.glob("**/src/wpt/**/*@/test.log"))
        if log_file.parent.name not in EXCLUDED_TESTS
    ]


def parse_log(log_file: Path, options: Options) -> Log:
    log = Log(suite=log_file.parent.as_posix().rpartition("/src/wpt/")[2])

    # This reads test.log rather than test.xml, because Bazel replaces each
    # 4-byte UTF-8 sequence with "????" when it copies the log into test.xml.
    text = log_file.read_text(encoding="utf-8", errors="surrogateescape")

    start_log = re.search(r"\[ TEST \] .*:zzz_results\n", text)
    if not start_log:
        return log

    end_log = re.search(r"^.*\[ (PASS|FAIL) \].*:zzz_results", text, re.MULTILINE)

    if not end_log:
        return log

    results_log = text[start_log.end(0) : end_log.start(0)]
    if not results_log.strip():
        # The suite ran without any of the GEN_TEST_* variables set.
        return log

    items = results_log.split("***")

    if options.config:
        # Removes a message for the user that we don't need
        items.pop(0)
        items.pop(0)
        log.config = items.pop(0).strip()

    if options.report:
        log.report = json.loads(items.pop(0))

    if options.stats:
        log.stats = json.loads(items.pop(0))

    return log


def stats_table(logs: list[Log]) -> str:
    cells = []

    for log in logs:
        if not log.stats:
            continue

        cells.append(
            [f"<td>{log.stats[0]}</td>"]
            + [f"<td align='right'>{value}</td>" for value in log.stats[1:]]
        )

    if not cells:
        return ""

    cells_html = "\n".join(f"<tr>{' '.join(row)}</tr>" for row in cells)

    return f"""## WPT statistics

<table>
    <tr>
        <th>Module</th>
        <th colspan="4">Coverage</th>
        <th colspan="5">Pass</th>
    </tr>
    <tr>
        <th></th>
        <th>OK</th>
        <th>Disabled</th>
        <th>Total</th>
        <th>% OK</th>
        <th>Pass</th>
        <th>Fail</th>
        <th>Disabled</th>
        <th>Total</th>
        <th>% Pass</th>
    </tr>
    {cells_html}
</table>

This table shows how workerd performs for each listed [Web Platform Tests](https://github.com/web-platform-tests/wpt) module."""


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


def workerd_version(workspace: str = ".") -> str:
    return cmd_output(["git", "-C", workspace, "describe", "--tags", "--always"])


def time_interval(logs: list[Log]) -> TimeInterval:
    """The span from the first suite's start to the last one's end.

    Falls back to the current time for reports from harnesses that don't
    record when a suite ran.
    """

    spans = [
        (log.report["time_start"], log.report["time_end"])
        for log in logs
        if log.report and "time_start" in log.report
    ]
    if not spans:
        now = int(time.time() * 1000)
        return TimeInterval(now, now)
    return TimeInterval(min(s for s, _ in spans), max(e for _, e in spans))


def wpt_report(
    results: list[dict[str, Any]],
    time_interval: TimeInterval,
    *,
    revision: str,
    browser_version: str,
    browser_channel: str = "experimental",
) -> dict[str, Any]:
    """A report in WPT's wptreport format, or {} when there are no results."""

    if not results:
        return {}

    return {
        "time_start": time_interval.start,
        "time_end": time_interval.end,
        "run_info": {
            "product": "workerd",
            "browser_channel": browser_channel,
            "browser_version": browser_version,
            "revision": revision,
            "os": get_os(),
        },
        "results": results,
    }


if __name__ == "__main__":
    main()
