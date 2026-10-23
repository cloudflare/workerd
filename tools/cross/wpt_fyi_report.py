#!/usr/bin/env python3
"""
Merges the per-suite WPT reports in a bazel testlogs directory into one
wptreport JSON file that wpt.fyi's results receiver accepts.

The suites must have run with GEN_TEST_REPORT=1 (`--config=wpt-test`).
Each result is renamed to the URL that WPT's MANIFEST.json gives the test
at the vendored revision, so that it lines up with other products on
wpt.fyi. Results that cannot be given such a URL are left out and counted
on stderr:

* files that the manifest does not list as testharness tests, such as
  helper scripts that the test configs mention;
* multi-global (.any.js) files that the manifest does not generate for any
  of the requested globals;
* files with META variants, because workerd runs every variant's subtests
  in a single pass, and the subtests cannot be attributed to a variant;
* files that the test config disables or omits as a whole, unless
  --keep-skipped is given.

Usage:
  wpt_fyi_report.py --manifest MANIFEST.json --wpt-revision SHA \\
      --output wptreport.json.gz bazel-testlogs/
"""

import argparse
import gzip
import json
import re
import sys
from collections import Counter
from pathlib import Path
from typing import Any, Optional

from wpt_logs import Options, cmd_output, get_os, parse_log

# Statuses that wpt.fyi's results processor knows how to abbreviate:
# https://github.com/web-platform-tests/wpt.fyi/blob/main/results-processor/wptreport.py
TEST_STATUSES = {"OK", "ERROR", "TIMEOUT", "CRASH", "SKIP", "PRECONDITION_FAILED"}
SUBTEST_STATUSES = {"PASS", "FAIL", "TIMEOUT", "NOTRUN", "PRECONDITION_FAILED"}

# Targets that test the same WPT directory as another target, so their
# results would collide. urlpattern@ tests the legacy implementation, and
# the -ts targets test the TypeScript streams implementation.
DEFAULT_EXCLUDED_TARGETS = r"^(urlpattern|.*-ts)@$"

# The suffixes that WPT gives each global of a multi-global test:
# https://web-platform-tests.org/writing-tests/testharness.html#multi-global-tests
ANY_GLOBAL_SUFFIXES = {
    "window": ".any.html",
    "worker": ".any.worker.html",
    "serviceworker": ".any.serviceworker.html",
    "sharedworker": ".any.sharedworker.html",
    "worker-module": ".any.worker-module.html",
    "serviceworker-module": ".any.serviceworker-module.html",
    "sharedworker-module": ".any.sharedworker-module.html",
}

REVISION_PATTERN = re.compile(r"^[0-9a-f]{40}$")


def main() -> None:
    cmd = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    cmd.add_argument("logs_dir", type=Path)
    cmd.add_argument(
        "--manifest",
        type=Path,
        required=True,
        help="MANIFEST.json of the vendored WPT revision (`wpt manifest`)",
    )
    cmd.add_argument(
        "--wpt-revision",
        required=True,
        help="full 40-character hash of the vendored WPT commit",
    )
    cmd.add_argument("--output", type=Path, required=True)
    cmd.add_argument(
        "--browser-version",
        help="workerd version; defaults to `git describe --tags --always`",
    )
    cmd.add_argument("--browser-channel", default="experimental")
    cmd.add_argument(
        "--any-global",
        default="worker,window,serviceworker,sharedworker",
        help="globals to report .any.js results under, in order of preference",
    )
    cmd.add_argument("--exclude-targets", default=DEFAULT_EXCLUDED_TARGETS)
    cmd.add_argument(
        "--keep-skipped",
        action="store_true",
        help="report wholly disabled or omitted files with status SKIP",
    )
    args = cmd.parse_args()

    if not REVISION_PATTERN.match(args.wpt_revision):
        sys.exit("--wpt-revision must be a full 40-character commit hash")

    globals_ = [g.strip() for g in args.any_global.split(",")]
    unknown = set(globals_) - ANY_GLOBAL_SUFFIXES.keys()
    if unknown:
        sys.exit(f"Unknown globals: {', '.join(sorted(unknown))}")

    manifest = Manifest(args.manifest)
    excluded = re.compile(args.exclude_targets)
    skipped: Counter[str] = Counter()
    results: dict[str, dict[str, Any]] = {}
    times: list[tuple[int, int]] = []

    for xml in sorted(args.logs_dir.glob("**/src/wpt/**/*@/test.xml")):
        target = xml.parent.name
        if excluded.match(target):
            skipped["excluded target"] += 1
            continue

        report = parse_log(xml, Options(config=False, report=True, stats=False))
        if not report.report:
            sys.exit(f"{xml} has no report; did the suite run with GEN_TEST_REPORT=1?")
        times.append(suite_time_ms(xml))

        for result in report.report["results"]:
            reason = convert_result(result, manifest, globals_, args.keep_skipped)
            if reason:
                skipped[reason] += 1
                continue
            if result["test"] in results:
                sys.exit(f"{result['test']} is reported by more than one suite")
            results[result["test"]] = result

    report = {
        "time_start": min(start for start, _ in times),
        "time_end": max(end for _, end in times),
        "run_info": {
            "product": "workerd",
            "browser_version": args.browser_version
            or cmd_output(["git", "describe", "--tags", "--always"]),
            "browser_channel": args.browser_channel,
            "os": get_os(),
            "revision": args.wpt_revision,
        },
        "results": sorted(results.values(), key=lambda r: r["test"]),
    }
    validate(report)

    data = json.dumps(report).encode()
    if args.output.suffix == ".gz":
        data = gzip.compress(data)
    args.output.write_bytes(data)

    subtests = Counter(s["status"] for r in report["results"] for s in r["subtests"])
    print(f"Wrote {len(results)} tests to {args.output}", file=sys.stderr)
    print(f"Subtests: {dict(subtests)}", file=sys.stderr)
    for reason, count in skipped.most_common():
        print(f"Left out {count}: {reason}", file=sys.stderr)


class Manifest:
    """Looks up the testharness URLs that MANIFEST.json gives a source file."""

    def __init__(self, path: Path) -> None:
        with path.open() as fp:
            self.testharness = json.load(fp)["items"]["testharness"]

    def urls(self, source: str) -> Optional[list[str]]:
        node = self.testharness
        for part in source.split("/"):
            if not isinstance(node, dict) or part not in node:
                return None
            node = node[part]
        if not isinstance(node, list):
            return None
        # A leaf is [hash, [url, extras], ...]; the url is null when it is
        # the source path itself.
        return ["/" + (url or source) for url, _ in node[1:]]


def convert_result(
    result: dict[str, Any],
    manifest: Manifest,
    globals_: list[str],
    keep_skipped: bool,
) -> Optional[str]:
    """Renames the result to its wpt.fyi test ID, or returns why it can't."""

    if result["status"] == "SKIP" and not keep_skipped:
        return "disabled or omitted file"

    # The harness names each result after its source file with .js replaced
    # by .html.
    source = result["test"].lstrip("/").removesuffix(".html") + ".js"
    urls = manifest.urls(source)
    if not urls:
        return "not a testharness test in the manifest"

    if source.endswith(".any.js"):
        stem = "/" + source.removesuffix(".any.js")
        for name in globals_:
            prefix = stem + ANY_GLOBAL_SUFFIXES[name]
            candidates = [u for u in urls if u.split("?")[0] == prefix]
            if candidates:
                break
        else:
            return "no requested global in the manifest"
    else:
        candidates = urls

    if len(candidates) > 1:
        # The harness runs every test with an empty query string. Tests
        # whose variants select a configuration, such as the websockets
        # tests' ?default, ?wss and ?wpt_flags=h2, treat an empty query
        # like ?default. Tests whose variants select a subset of subtests
        # run all of them, so no single variant describes the result.
        candidates = [u for u in candidates if u.endswith("?default")]
        if len(candidates) != 1:
            return "has META variants"

    result["test"] = candidates[0]
    return None


def suite_time_ms(xml: Path) -> tuple[int, int]:
    """Approximates a suite's run as the time its log was written.

    Bazel's generated test.xml carries no timestamps, so this is a lower
    bound on the run's extent rather than its true start.
    """

    end = int(xml.with_name("test.log").stat().st_mtime * 1000)
    return end, end


def validate(report: dict[str, Any]) -> None:
    """Checks what wpt.fyi's receiver and results processor require."""

    for key in ("product", "browser_version", "os", "revision"):
        assert report["run_info"].get(key), f"run_info.{key} is required"
    assert report["results"], "results must not be empty"

    for result in report["results"]:
        assert result["test"].startswith("/"), result["test"]
        assert result["status"] in TEST_STATUSES, result
        names = [s["name"] for s in result["subtests"]]
        if len(names) != len(set(names)):
            dups = [n for n, c in Counter(names).items() if c > 1]
            print(f"warning: {result['test']} repeats subtests {dups}", file=sys.stderr)
        for subtest in result["subtests"]:
            assert subtest["status"] in SUBTEST_STATUSES, (result["test"], subtest)


if __name__ == "__main__":
    main()
