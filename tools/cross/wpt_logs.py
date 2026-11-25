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
from xml.etree import ElementTree

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
    cmd.add_argument("--write-report", type=Path)
    cmd.add_argument("--print-stats", action="store_true")
    cmd.add_argument("logs_dir", type=Path)

    args = cmd.parse_args()
    options = Options(args.update_config, bool(args.write_report), args.print_stats)
    logs = parse_logs(args.logs_dir, options)

    if args.print_stats:
        print(stats_table(logs))

    if args.write_report:
        # TODO(soon): Elapsed time will not be accurate. Figure out if it matters to us, and how to fix.
        now = int(time.time())
        time_interval = TimeInterval(now, now)

        report = wpt_report(logs, time_interval)
        if report:
            with args.write_report.open("w") as fp:
                json.dump(report, fp, indent=2)

    # TODO(soon): Implement the config option to update test configs


@dataclass
class TimeInterval:
    start: int = 0
    end: int = 0


@dataclass
class Log:
    # TypeScript test config used to configure WPT tests
    config: Optional[str] = None

    # JSON report in WPT's format
    report: Optional[dict[str, Any]] = None

    # Summary stats in JSON
    stats: Optional[list[Any]] = None


def parse_logs(logs_dir: Path, options: Options) -> list[Log]:
    # A sharded target (shard_count) has a log in a shard_<n>_of_<count> directory for each shard.
    log_files = list(logs_dir.glob("**/src/wpt/**/*@/test.xml")) + list(
        logs_dir.glob("**/src/wpt/**/*@/shard_*_of_*/test.xml")
    )
    return [parse_log(log_file, options) for log_file in sorted(log_files)]


def parse_log(log_file: Path, options: Options) -> Log:
    log = Log()

    target_dir = log_file.parent
    if target_dir.name.startswith("shard_"):
        target_dir = target_dir.parent
    if target_dir.name in EXCLUDED_TESTS:
        return log

    parser = ElementTree.XMLParser(encoding="utf-8")
    root = ElementTree.parse(log_file, parser=parser)
    text = root.find("testsuite").find("system-out").text

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
        log.stats = json.loads(items.pop(0))

    return log


def percent(part: int, total: int) -> str:
    return f"{part / total * 100:.0f}" if total else "NaN"


def merge_stats(logs: list[Log]) -> list[list[Any]]:
    """
    Sum the stats of each module over its shards.

    Each row is [module, ok, disabled, total, % ok, pass, fail, disabled, total, % pass].
    """

    merged: dict[str, list[Any]] = {}

    for log in logs:
        if not log.stats:
            continue

        module, *values = log.stats
        if module not in merged:
            merged[module] = [module, *values]
            continue

        row = merged[module]
        for i in (1, 2, 3, 5, 6, 7, 8):
            row[i] += log.stats[i]
        row[4] = percent(row[1], row[3])
        row[9] = percent(row[5], row[8])

    return list(merged.values())


def stats_table(logs: list[Log]) -> str:
    cells = [
        [f"<td>{stats[0]}</td>"]
        + [f"<td align='right'>{value}</td>" for value in stats[1:]]
        for stats in merge_stats(logs)
    ]

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


def wpt_report(logs: list[Log], time_interval: TimeInterval) -> dict[str, Any]:
    all_results = [
        result for log in logs if log.report for result in log.report["results"]
    ]

    if not all_results:
        return {}

    return {
        "time_start": time_interval.start,
        "time_end": time_interval.end,
        "run_info": {
            "product": "workerd",
            "browser_channel": "experimental",
            "browser_version": cmd_output(["git", "describe", "--tags", "--always"]),
            "revision": cmd_output(["git", "rev-parse", "HEAD"]),
            "os": get_os(),
        },
        "results": all_results,
    }


if __name__ == "__main__":
    main()
