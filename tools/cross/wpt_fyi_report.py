#!/usr/bin/env python3
"""
Merges the per-suite WPT reports in a bazel testlogs directory into one
wptreport JSON file that wpt.fyi's results receiver accepts.

The suites must have run with GEN_TEST_REPORT=1 (`--config=wpt-test`).

workerd's harness does not run tests the way WPT does. WPT runs each test
URL separately: each global that a file's `// META: global=` asks for, and
each of its `// META: variant=` query strings, is a test of its own with its
own ID on wpt.fyi. workerd runs each file once, in workerd's own global
scope, which is none of WPT's, without a query string, and runs every
subtest. The report labels that one run as one of WPT's globals, the first
of --any-global that the file asks for, and reports a file whose variants
select a configuration rather than subtests as its ?default variant. So a
file's rows on wpt.fyi approximate WPT's runs; they are not separate runs. If the harness ran each test URL the
way WPT defines it, each variant with its query string, under a deliberately
chosen global, the test IDs could come straight from the MANIFEST.json that
WPT publishes, which would be more faithful than deriving them here.

The test IDs follow WPT's rules for naming the tests of a script file, in
tools/manifest/sourcefile.py, from the file's path and the globals and
variants that the harness reads from its META comments. Results that cannot
be given an ID are left out and counted on stderr:

* files that WPT does not treat as tests, such as helper scripts that the
  test configs mention;
* multi-global (.any.js) files that ask for none of the --any-global
  globals;
* files with META variants and no ?default one, because workerd runs every
  variant's subtests in a single pass, and the subtests cannot be attributed
  to a variant;
* files that the test config disables or omits as a whole, unless
  --keep-skipped is given;
* the suites that src/wpt/BUILD.bazel marks `wpt_fyi = False`, because
  another suite runs the same files. Two reported suites that give the same
  test ID are an error.

--wpt-revision-file holds the full hash of the tested WPT commit. `bazel run
//tools:wpt_fyi_report` passes the REVISION file of the pinned WPT release,
so it fails to build for a release that has none (see docs/wpt-fyi.md).
Relative paths in the other arguments are taken from the directory that
Bazel was run in.

wpt.fyi only accepts gzipped reports; an --output name that does not end in
.gz is written uncompressed, for reading.
"""

import argparse
import gzip
import json
import os
import posixpath
import re
import sys
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Optional, Union

from wpt_logs import (
    Log,
    Options,
    parse_logs,
    time_interval,
    workerd_version,
    wpt_report,
)

# Statuses that wpt.fyi's results processor knows how to abbreviate:
# https://github.com/web-platform-tests/wpt.fyi/blob/main/results-processor/wptreport.py
# The suffixes that WPT gives each global of a multi-global test, by the
# names that --any-global uses:
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

# The globals that `// META: global=` can name, from _any_variants in WPT's
# tools/manifest/sourcefile.py: the global types that each name stands for,
# and the suffixes of the types' test URLs, which are .any.<type>.html for
# the types not listed.
WPT_GLOBAL_TYPES = {
    "worker": {"dedicatedworker", "sharedworker", "serviceworker"},
    "shadowrealm": {
        "shadowrealm-in-window",
        "shadowrealm-in-shadowrealm",
        "shadowrealm-in-dedicatedworker",
        "shadowrealm-in-sharedworker",
        "shadowrealm-in-serviceworker",
        "shadowrealm-in-audioworklet",
    },
} | {
    name: {name}
    for name in (
        "window",
        "window-module",
        "serviceworker",
        "serviceworker-module",
        "sharedworker",
        "sharedworker-module",
        "dedicatedworker",
        "dedicatedworker-module",
        "worker-module",
        "shadowrealm-in-window",
        "shadowrealm-in-shadowrealm",
        "shadowrealm-in-dedicatedworker",
        "shadowrealm-in-sharedworker",
        "shadowrealm-in-serviceworker",
        "shadowrealm-in-audioworklet",
        "jsshell",
    )
}
WPT_GLOBAL_SUFFIXES = {
    "window": ".any.html",
    "dedicatedworker": ".any.worker.html",
    "dedicatedworker-module": ".any.worker-module.html",
    "shadowrealm-in-serviceworker": ".https.any.shadowrealm-in-serviceworker.html",
    "shadowrealm-in-audioworklet": ".https.any.shadowrealm-in-audioworklet.html",
    "jsshell": ".any.js",
}
# The global types of a multi-global test without `// META: global=`
DEFAULT_GLOBAL_TYPES = {"window", "dedicatedworker"}

# Directories whose files WPT treats as support files rather than tests
NON_TEST_DIRS = {"resources", "support", "tools"}

# Under `bazel run`, the program runs in its runfiles directory.
WORKING_DIRECTORY = os.environ.get("BUILD_WORKING_DIRECTORY", ".")
WORKSPACE = os.environ.get("BUILD_WORKSPACE_DIRECTORY", ".")


def working_path(path: str) -> Path:
    """Resolves a path given on the command line."""
    return Path(WORKING_DIRECTORY, path)


def parse_args() -> argparse.Namespace:
    cmd = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    cmd.add_argument("logs_dir", type=working_path)
    cmd.add_argument(
        "--wpt-revision-file",
        type=Path,
        required=True,
        help="file holding the full hash of the tested WPT commit "
        "(an absolute path, under bazel run)",
    )
    cmd.add_argument("--output", type=working_path, required=True)
    cmd.add_argument(
        "--browser-version",
        help="workerd version; defaults to `git describe --tags --always`",
    )
    cmd.add_argument(
        "--browser-channel",
        default="experimental",
        help="release channel; wpt.fyi labels the run with it",
    )
    cmd.add_argument(
        "--any-global",
        type=any_globals,
        default="worker,window,serviceworker,sharedworker",
        help="globals to report .any.js results under, in order of preference",
    )
    cmd.add_argument(
        "--keep-skipped",
        action="store_true",
        help="report wholly disabled or omitted files with status SKIP",
    )
    return cmd.parse_args()


def any_globals(value: str) -> list[str]:
    globals_ = [g.strip() for g in value.split(",")]
    unknown = set(globals_) - ANY_GLOBAL_SUFFIXES.keys()
    if unknown:
        raise argparse.ArgumentTypeError(
            f"unknown globals: {', '.join(sorted(unknown))}"
        )
    return globals_


@dataclass
class Collected:
    """The results of the reported suites in a testlogs directory."""

    results: dict[str, dict[str, Any]] = field(default_factory=dict)
    logs: list[Log] = field(default_factory=list)
    skipped: Counter[str] = field(default_factory=Counter)
    left_out_suites: list[str] = field(default_factory=list)


def collect(logs_dir: Path, globals_: list[str], keep_skipped: bool) -> Collected:
    """Converts the results of every reported suite under `logs_dir`."""

    collected = Collected()
    reported_by: dict[str, str] = {}
    for log in parse_logs(logs_dir, Options(config=False, report=True, stats=False)):
        if not log.report:
            sys.exit(f"{log.suite} has no report; did it run with GEN_TEST_REPORT=1?")
        if not log.report["wpt_fyi"]:
            collected.left_out_suites.append(log.suite)
            continue
        collected.logs.append(log)

        for result in log.report["results"]:
            converted = convert_result(result, globals_, keep_skipped)
            if isinstance(converted, str):
                collected.skipped[converted] += 1
                continue
            for test in converted:
                if test["test"] in collected.results:
                    sys.exit(
                        f"{test['test']} is reported by both {reported_by[test['test']]} "
                        f"and {log.suite}; mark one of the suites wpt_fyi = False in "
                        "src/wpt/BUILD.bazel"
                    )
                collected.results[test["test"]] = test
                reported_by[test["test"]] = log.suite

    if not collected.logs:
        sys.exit(f"No WPT suites in {logs_dir}")
    return collected


def write_report(report: dict[str, Any], output: Path) -> None:
    data = json.dumps(report).encode()
    if output.suffix == ".gz":
        data = gzip.compress(data, mtime=0)
    output.write_bytes(data)


def print_summary(collected: Collected, report: dict[str, Any], output: Path) -> None:
    subtests = Counter(s["status"] for r in report["results"] for s in r["subtests"])
    print(
        f"Wrote {len(report['results'])} tests from {len(collected.logs)} suites to {output}",
        file=sys.stderr,
    )
    print(f"WPT revision: {report['run_info']['revision']}", file=sys.stderr)
    print(f"Subtests: {dict(subtests)}", file=sys.stderr)
    for reason, count in collected.skipped.most_common():
        print(f"Left out {count}: {reason}", file=sys.stderr)
    if collected.left_out_suites:
        print(
            "Left out the suites marked wpt_fyi = False: "
            + ", ".join(collected.left_out_suites),
            file=sys.stderr,
        )


def main() -> None:
    args = parse_args()
    collected = collect(args.logs_dir, args.any_global, args.keep_skipped)
    report = wpt_report(
        sorted(collected.results.values(), key=lambda r: r["test"]),
        time_interval(collected.logs),
        revision=args.wpt_revision_file.read_text().strip(),
        browser_version=args.browser_version or workerd_version(WORKSPACE),
        browser_channel=args.browser_channel,
    )
    write_report(report, args.output)
    print_summary(collected, report, args.output)


def test_urls(
    source: str, globals_: Optional[list[str]], variants: Optional[list[str]]
) -> list[str]:
    """Returns the URLs of the tests that WPT makes of a script file.

    This follows SourceFile.manifest_items() in WPT's
    tools/manifest/sourcefile.py for .js files, which are what the harness
    runs. `globals_` and `variants` are the values of the file's
    `// META: global=` and `// META: variant=` comments. A file that WPT
    does not treat as a testharness test has none.
    """

    parts = source.split("/")
    filename = parts[-1]
    if (
        len(parts) == 1
        or parts[0] in ("common", "conformance-checkers")
        or NON_TEST_DIRS.intersection(parts)
        or filename.startswith(".")
    ):
        return []

    name, ext = posixpath.splitext(filename)
    if ext != ".js":
        return []
    if "-" in name:
        type_flag, *flags = name.rsplit("-", 1)[1].split(".")
    else:
        type_flag, flags = None, name.split(".")[1:]
    if type_flag in ("manual", "visual"):
        return []

    if "any" in flags:
        if globals_ is None:
            types = DEFAULT_GLOBAL_TYPES
        else:
            types = set().union(
                *(WPT_GLOBAL_TYPES.get(g.strip(), set()) for g in globals_)
            )
        stem = source.replace(".any.", ".")
        bases = []
        for type_ in sorted(types):
            suffix = WPT_GLOBAL_SUFFIXES.get(type_, f".any.{type_}.html")
            base = stem
            for flag in (".https.", ".h2."):
                if flag in base and suffix.startswith(flag):
                    base = base.replace(flag, ".")
                    break
            bases.append(base.removesuffix(".js") + suffix)
    elif "worker" in flags or "window" in flags or "extension" in flags:
        bases = [source.removesuffix(".js") + ".html"]
    else:
        return []

    return ["/" + base + variant for base in bases for variant in variants or [""]]


def convert_result(
    result: dict[str, Any],
    globals_: list[str],
    keep_skipped: bool,
) -> Union[list[dict[str, Any]], str]:
    """Returns the result under its wpt.fyi test ID, or why it has none."""

    if result["status"] == "SKIP" and not keep_skipped:
        return "disabled or omitted file"

    # The harness names each result after its source file with .js replaced
    # by .html.
    source = result["test"].lstrip("/").removesuffix(".html") + ".js"
    urls = test_urls(source, result.get("globals"), result.get("variants"))
    if not urls:
        return "not a testharness test"

    if source.endswith(".any.js"):
        stem = "/" + source.removesuffix(".any.js")
        for name in globals_:
            prefix = stem + ANY_GLOBAL_SUFFIXES[name]
            candidates = [u for u in urls if u.split("?")[0] == prefix]
            if candidates:
                break
        else:
            return "asks for none of the requested globals"
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

    return [renamed(result, candidates[0])]


# Fields that the harness adds to results for this program, which are not
# part of wptreport
HARNESS_FIELDS = {"globals", "variants"}


def renamed(result: dict[str, Any], test: str) -> dict[str, Any]:
    """Returns a copy of the result named `test`, without the harness's fields."""

    fields = {k: v for k, v in result.items() if k not in HARNESS_FIELDS}
    return {**fields, "test": test}


if __name__ == "__main__":
    main()
