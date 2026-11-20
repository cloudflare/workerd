#!/usr/bin/env python3
"""Tracks flaky Bazel tests in CI as GitHub issues.

A test is flaky when it fails in one attempt of a CI job and passes in another attempt of the job,
which GitHub runs on the same commit. The workflows run three commands:

- `extract`, in each test job of _bazel.yml: reduces Bazel's build event file to the status of each
  test, which the job uploads as its `test-results-<job>-attempt<N>` artifact.
- `summarize`, in a test job whose tests failed: notes which of the failures are known to be flaky.
- `record`, in flaky-tests.yml: compares the attempts of each re-run test job, and keeps one issue
  per flaky test, labelled `flaky-test`, and a dashboard issue that ranks them.

The artifacts come from pull requests, including ones from forks, so `record` treats their contents
as untrusted data: it accepts only well-formed labels in packages that exist on the default branch,
and limits how many issues one run can open.
"""

from __future__ import annotations

import argparse
import dataclasses
import io
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request
import zipfile
from collections import Counter, defaultdict
from collections.abc import Callable, Iterable, Iterator
from datetime import UTC, datetime, timedelta
from pathlib import Path
from typing import Any, TextIO

ISSUE_LABEL = "flaky-test"
RESULTS_FILE = "test-results.json"
RESULTS_VERSION = 1
TESTS_WORKFLOW = ".github/workflows/test.yml"
WORKFLOW_URL = "https://github.com/{repo}/blob/main/.github/workflows/flaky-tests.yml"

# The artifact a test job uploads for each of its attempts (see _bazel.yml).
ARTIFACT_RE = re.compile(r"\Atest-results-([A-Za-z0-9_-]+)-attempt([1-9][0-9]*)\Z")

# Bazel's TestStatus values.
STATUSES = frozenset(
    {
        "NO_STATUS",
        "PASSED",
        "FLAKY",
        "TIMEOUT",
        "FAILED",
        "INCOMPLETE",
        "REMOTE_FAILURE",
        "FAILED_TO_BUILD",
        "TOOL_HALTED_BEFORE_TESTING",
    }
)
FAILED = frozenset({"FAILED", "TIMEOUT"})
# FLAKY is a test that Bazel retried until it passed.
PASSED = frozenset({"PASSED", "FLAKY"})

# A test in this repository, `//package:target`, and the variant suffix that build/wd_test.bzl and
# build/kj_test.bzl add to its name (`@`, `@all-autogates`, `@all-compat-flags`), if any.
LABEL_RE = re.compile(
    r"\A(?P<target>//(?P<package>[A-Za-z0-9_.+-]+(?:/[A-Za-z0-9_.+-]+)*)?"
    r":[A-Za-z0-9_.+=,~-]+(?:/[A-Za-z0-9_.+=,~-]+)*)(?P<variant>@[A-Za-z0-9_-]*)?\Z"
)

ISSUE_MARKER = "<!-- flaky-test: {target} -->"
ISSUE_MARKER_RE = re.compile(r"<!-- flaky-test: (\S+) -->")
DATA_RE = re.compile(r"<!-- flaky-test-data\r?\n(.*?)\r?\n-->", re.DOTALL)
DASHBOARD_MARKER = "<!-- flaky-test-dashboard -->"
DASHBOARD_TITLE = "Flaky test dashboard"
DASHBOARD_DAYS = 30

MAX_DOWNLOAD_BYTES = 32 << 20
# An issue keeps this many of its test's latest failures, and shows fewer.
MAX_STORED_OCCURRENCES = 100
MAX_TABLE_ROWS = 30
MAX_SUMMARY_ROWS = 100
# The most issues that one run's artifacts can open. A pull request controls its run's artifacts.
MAX_NEW_ISSUES_PER_RUN = 10

# The errors of an artifact that can't be downloaded or read, which `record` skips. OSError
# includes urllib's errors, as for an artifact that expired after it was listed.
UNUSABLE_ARTIFACT_ERRORS = (
    OSError,
    TypeError,
    ValueError,
    KeyError,
    zipfile.BadZipFile,
)


def split_label(label: str) -> tuple[str, str]:
    """Splits a test label into its target and variant, which is "" for a test without one."""
    match = LABEL_RE.match(label)
    if match is None:
        raise ValueError(f"not a test label: {label!r}")
    return match["target"], match["variant"] or ""


def package_exists(repo_root: Path, label: str) -> bool:
    match = LABEL_RE.match(label)
    if match is None:
        return False
    package = match["package"] or ""
    if any(part in (".", "..") for part in package.split("/")):
        return False
    directory = repo_root / package
    return (directory / "BUILD.bazel").is_file() or (directory / "BUILD").is_file()


def utc_iso(moment: datetime) -> str:
    """Formats a time as GitHub does, so that the strings sort by time."""
    return moment.astimezone(UTC).strftime("%Y-%m-%dT%H:%M:%SZ")


# ---------------------------------------------------------------------------------------------
# Test results
# ---------------------------------------------------------------------------------------------


def read_test_statuses(lines: Iterable[str]) -> dict[str, str]:
    """Returns the overall status of each test in a build event file."""
    statuses: dict[str, str] = {}
    for line in lines:
        # Most events, such as the ones that hold the build's console output, are not test
        # summaries, so they are skipped without parsing them.
        if '"testSummary"' not in line:
            continue
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            # A job that is stopped while Bazel writes the file leaves a partial last line.
            continue
        summary_id = event.get("id", {}).get("testSummary")
        if summary_id is None:
            continue
        label = summary_id.get("label")
        status = event.get("testSummary", {}).get("overallStatus", "NO_STATUS")
        # A test built in more than one configuration keeps a failure.
        if isinstance(label, str) and statuses.get(label) not in FAILED:
            statuses[label] = status
    return statuses


def extract(bep_file: Path, output: Path) -> None:
    if not bep_file.is_file():
        print(f"No build event file at {bep_file}: Bazel ran no tests.")
        return
    with bep_file.open(encoding="utf-8", errors="replace") as lines:
        statuses = read_test_statuses(lines)
    output.write_text(
        json.dumps(
            {"version": RESULTS_VERSION, "tests": statuses}, indent=0, sort_keys=True
        ),
        encoding="utf-8",
    )
    failed = sum(status in FAILED for status in statuses.values())
    print(f"Recorded the results of {len(statuses)} tests, {failed} of which failed.")


def parse_results(data: Any) -> dict[str, str]:
    """Validates the contents of a results artifact, dropping any test it can't use."""
    if not isinstance(data, dict):
        raise TypeError("the results are not a JSON object")
    if data.get("version") != RESULTS_VERSION:
        raise ValueError(f"unknown results version {data.get('version')!r}")
    tests = data.get("tests")
    if not isinstance(tests, dict):
        raise TypeError("the results' tests are not a JSON object")
    return {
        label: status
        for label, status in tests.items()
        if isinstance(label, str)
        and LABEL_RE.match(label)
        and isinstance(status, str)
        and status in STATUSES
    }


def parse_results_zip(data: bytes) -> dict[str, str]:
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        info = archive.getinfo(RESULTS_FILE)
        if info.file_size > MAX_DOWNLOAD_BYTES:
            raise ValueError(f"{RESULTS_FILE} is {info.file_size} bytes")
        with archive.open(info) as file:
            raw = file.read(MAX_DOWNLOAD_BYTES + 1)
    if len(raw) > MAX_DOWNLOAD_BYTES:
        raise ValueError(f"{RESULTS_FILE} is too large")
    return parse_results(json.loads(raw))


def flaky_failures(results: dict[int, dict[str, str]]) -> list[tuple[str, str, int]]:
    """Finds the failures in a job's attempts of tests that another attempt of the job passed.

    `results` maps each attempt to the status of each of its tests. Returns the label, status and
    attempt of each such failure.
    """
    found = []
    for attempt, statuses in results.items():
        others = [other for number, other in results.items() if number != attempt]
        for label, status in statuses.items():
            if status in FAILED and any(other.get(label) in PASSED for other in others):
                found.append((label, status, attempt))
    return sorted(found)


# ---------------------------------------------------------------------------------------------
# Issues
# ---------------------------------------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class Occurrence:
    """A failure of a test in one attempt of a job, which another attempt of the job passed."""

    label: str
    job: str
    status: str
    run_id: int
    attempt: int
    sha: str
    # When the failing attempt uploaded its results.
    time: str
    # "push" for a run on main, else "pull_request".
    event: str
    # None for a run on main, and for a pull request from a fork, which GitHub doesn't link to the
    # runs that test it.
    pull_request: int | None

    @property
    def key(self) -> tuple[int, str, str, int]:
        return (self.run_id, self.job, self.label, self.attempt)

    @property
    def target(self) -> str:
        return split_label(self.label)[0]

    @property
    def variant(self) -> str:
        return split_label(self.label)[1]


@dataclasses.dataclass
class FlakyIssue:
    """The issue that tracks one flaky test, across all of its variants."""

    target: str
    number: int | None = None
    # None while the issue is open.
    closed_at: str | None = None
    # Latest first.
    occurrences: list[Occurrence] = dataclasses.field(default_factory=list)
    # How many failures the issue has recorded, including those it no longer keeps.
    total: int = 0
    changed: bool = False
    reopened: bool = False

    def add(self, occurrence: Occurrence) -> bool:
        if any(existing.key == occurrence.key for existing in self.occurrences):
            return False
        self.occurrences.append(occurrence)
        self.occurrences.sort(key=lambda o: (o.time, o.key), reverse=True)
        del self.occurrences[MAX_STORED_OCCURRENCES:]
        self.total += 1
        self.changed = True
        if self.closed_at is not None and occurrence.time > self.closed_at:
            self.closed_at = None
            self.reopened = True
        return True


def parse_issue(issue: dict[str, Any]) -> FlakyIssue | None:
    body = issue.get("body") or ""
    marker = ISSUE_MARKER_RE.search(body)
    if marker is None:
        return None
    flaky = FlakyIssue(
        target=marker[1],
        number=issue["number"],
        closed_at=issue.get("closed_at") if issue.get("state") == "closed" else None,
    )
    data = DATA_RE.search(body)
    if data is not None:
        try:
            stored = json.loads(data[1])
            flaky.occurrences = [Occurrence(**o) for o in stored["occurrences"]]
            flaky.total = max(int(stored["total"]), len(flaky.occurrences))
        except (ValueError, KeyError, TypeError) as error:
            print(
                f"Issue #{flaky.number} has unreadable failure data, which it drops: {error}"
            )
            flaky.occurrences = []
    return flaky


def data_block(flaky: FlakyIssue) -> str:
    data = {
        "total": flaky.total,
        "occurrences": [dataclasses.asdict(o) for o in flaky.occurrences],
    }
    # `>` occurs only inside JSON strings, where a Unicode escape can stand for it. Escaping it keeps
    # the data from ending the comment that holds it.
    text = json.dumps(data, sort_keys=True).replace(">", "\\u003e")
    return f"<!-- flaky-test-data\n{text}\n-->"


def describe_trigger(occurrence: Occurrence) -> str:
    # Not `#123`, which would add a reference to this issue to the pull request's timeline.
    if occurrence.event == "push":
        return "main"
    if occurrence.pull_request is not None:
        return f"PR {occurrence.pull_request}"
    return "pull request"


def render_issue(flaky: FlakyIssue, repo: str) -> str:
    latest = flaky.occurrences[0]
    rows = [
        "| {when} | `{job}` | {variant} | {status} | [attempt {attempt}]({run_url}) | {sha} | "
        "{trigger} |".format(
            when=o.time[:16].replace("T", " "),
            job=o.job,
            variant=f"`{o.variant}`" if o.variant else "-",
            status=o.status,
            attempt=o.attempt,
            run_url=f"https://github.com/{repo}/actions/runs/{o.run_id}/attempts/{o.attempt}",
            sha=o.sha[:10],
            trigger=describe_trigger(o),
        )
        for o in flaky.occurrences[:MAX_TABLE_ROWS]
    ]
    times = "once" if flaky.total == 1 else f"{flaky.total} times"
    lines = [
        ISSUE_MARKER.format(target=flaky.target),
        f"`{flaky.target}` failed in a CI job and passed in another attempt of the job, on the "
        f"same commit. It has failed this way {times}, most recently on {latest.time[:10]} in "
        f"`{latest.job}`.",
        "",
        "| Failed (UTC) | Job | Variant | Result | Run | Commit | Tested |",
        "| --- | --- | --- | --- | --- | --- | --- |",
        *rows,
    ]
    if flaky.total > len(rows):
        lines += [
            "",
            f"The table shows the latest {len(rows)} of {flaky.total} failures.",
        ]
    lines += [
        "",
        f"The [flaky-tests workflow]({WORKFLOW_URL.format(repo=repo)}) maintains this description "
        "and rewrites it after each new failure, so please comment instead of editing it. If the "
        "test fails this way after the issue is closed, the workflow reopens the issue.",
        "",
        data_block(flaky),
        "",
    ]
    return "\n".join(lines)


def render_dashboard(issues: Iterable[FlakyIssue], repo: str, now: datetime) -> str:
    month = utc_iso(now - timedelta(days=DASHBOARD_DAYS))
    week = utc_iso(now - timedelta(days=7))
    rows = []
    job_failures: Counter[str] = Counter()
    job_tests: defaultdict[str, set[str]] = defaultdict(set)
    for flaky in issues:
        if flaky.closed_at is not None:
            continue
        recent = [o for o in flaky.occurrences if o.time >= month]
        if not recent:
            continue
        jobs = Counter(o.job for o in recent)
        for job, count in jobs.items():
            job_failures[job] += count
            job_tests[job].add(flaky.target)
        rows.append(
            (
                -len(recent),
                flaky.target,
                "| `{target}` | {issue} | {week} | {month} | {jobs} | {last} |".format(
                    target=flaky.target,
                    issue=f"#{flaky.number}" if flaky.number is not None else "(new)",
                    week=sum(o.time >= week for o in recent),
                    month=len(recent),
                    jobs=", ".join(
                        f"`{job}` ({count})" for job, count in jobs.most_common()
                    ),
                    last=recent[0].time[:10],
                ),
            )
        )
    label_url = f"https://github.com/{repo}/issues?q=label%3A{ISSUE_LABEL}"
    lines = [
        DASHBOARD_MARKER,
        f"Bazel tests that failed in a CI job and passed in another attempt of the job, on the "
        f"same commit, over the last {DASHBOARD_DAYS} days. Each test has an open "
        f"[`{ISSUE_LABEL}`]({label_url}) issue that lists its failures. The "
        f"[flaky-tests workflow]({WORKFLOW_URL.format(repo=repo)}) maintains this issue.",
        "",
    ]
    if not rows:
        lines += [f"No test has failed this way in the last {DASHBOARD_DAYS} days.", ""]
        return "\n".join(lines)
    lines += [
        "| Test | Issue | 7 days | 30 days | Jobs (30 days) | Last failed |",
        "| --- | --- | --- | --- | --- | --- |",
        *(row for _, _, row in sorted(rows)),
        "",
        "| Job | Failures (30 days) | Tests |",
        "| --- | --- | --- |",
        *(
            f"| `{job}` | {count} | {len(job_tests[job])} |"
            for job, count in sorted(
                job_failures.items(), key=lambda item: (-item[1], item[0])
            )
        ),
        "",
    ]
    return "\n".join(lines)


# ---------------------------------------------------------------------------------------------
# GitHub
# ---------------------------------------------------------------------------------------------


class GitHub:
    """A minimal client for GitHub's REST API."""

    def __init__(
        self, repo: str, token: str, api_url: str = "https://api.github.com"
    ) -> None:
        self.repo = repo
        self._token = token
        self._api_url = api_url

    def get(self, path: str) -> Any:
        return json.loads(self._request("GET", self._api_url + "/" + path)[0])

    def paginate(self, path: str, key: str | None = None) -> Iterator[Any]:
        url: str | None = self._api_url + "/" + path
        while url is not None:
            raw, headers = self._request("GET", url)
            page = json.loads(raw)
            yield from page[key] if key is not None else page
            url = next_page(headers.get("Link"))

    def post(self, path: str, body: dict[str, Any]) -> Any:
        return json.loads(self._request("POST", self._api_url + "/" + path, body)[0])

    def patch(self, path: str, body: dict[str, Any]) -> Any:
        return json.loads(self._request("PATCH", self._api_url + "/" + path, body)[0])

    def download(self, path: str) -> bytes:
        return self._request("GET", self._api_url + "/" + path)[0]

    def _request(
        self, method: str, url: str, body: dict[str, Any] | None = None
    ) -> tuple[bytes, Any]:
        data = json.dumps(body).encode() if body is not None else None
        for attempt in range(3):
            request = urllib.request.Request(url, data=data, method=method)
            request.add_header("Accept", "application/vnd.github+json")
            request.add_header("X-GitHub-Api-Version", "2022-11-28")
            request.add_header("User-Agent", "workerd-flaky-tests")
            if data is not None:
                request.add_header("Content-Type", "application/json")
            # Not passed on to the storage service that an artifact download redirects to.
            request.add_unredirected_header("Authorization", f"Bearer {self._token}")
            try:
                with urllib.request.urlopen(request, timeout=60) as response:
                    raw = response.read(MAX_DOWNLOAD_BYTES + 1)
                    if len(raw) > MAX_DOWNLOAD_BYTES:
                        raise ValueError(
                            f"{url} returned more than {MAX_DOWNLOAD_BYTES} bytes"
                        )
                    return raw, response.headers
            except urllib.error.HTTPError as error:
                # Only a read is safe to repeat.
                if method != "GET" or error.code < 500 or attempt == 2:
                    raise
            except urllib.error.URLError:
                if method != "GET" or attempt == 2:
                    raise
            time.sleep(2**attempt)
        raise AssertionError("unreachable")


def next_page(link: str | None) -> str | None:
    for part in (link or "").split(","):
        match = re.match(r'\s*<([^>]+)>;\s*rel="next"', part)
        if match:
            return match[1]
    return None


def list_issues(gh: Any, state: str) -> Iterator[dict[str, Any]]:
    path = f"repos/{gh.repo}/issues?labels={ISSUE_LABEL}&state={state}&per_page=100"
    # The issues API lists pull requests too.
    return (issue for issue in gh.paginate(path) if "pull_request" not in issue)


# ---------------------------------------------------------------------------------------------
# summarize
# ---------------------------------------------------------------------------------------------


def known_flaky_tests(gh: Any) -> dict[str, int]:
    known = {}
    for issue in list_issues(gh, "open"):
        flaky = parse_issue(issue)
        if flaky is not None:
            known.setdefault(flaky.target, issue["number"])
    return known


def escape_command(text: str) -> str:
    return text.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def summarize(results: Path, gh: Any, summary: TextIO, log: TextIO) -> None:
    if not results.is_file():
        return
    statuses = parse_results(json.loads(results.read_text(encoding="utf-8")))
    failures = sorted(
        (label, status) for label, status in statuses.items() if status in FAILED
    )
    if not failures:
        return
    try:
        known = known_flaky_tests(gh)
    except (OSError, ValueError) as error:
        print(f"::warning::Could not look up the known flaky tests: {error}", file=log)
        known = None

    def issue_url(number: int) -> str:
        return f"https://github.com/{gh.repo}/issues/{number}"

    rows = []
    flaky_count = 0
    for label, status in failures:
        number = known.get(split_label(label)[0]) if known is not None else None
        if number is not None:
            flaky_count += 1
            print(
                "::warning title=Known flaky test::"
                + escape_command(
                    f"{label} failed, and is a known flaky test: {issue_url(number)}"
                ),
                file=log,
            )
        known_cell = f"[#{number}]({issue_url(number)})" if number is not None else ""
        rows.append(f"| `{label}` | {status} | {known_cell} |")

    lines = ["### Failed tests", ""]
    if known is None:
        lines.append("Could not look up which of these tests are known to be flaky.")
    elif flaky_count:
        lines.append(
            f"{flaky_count} of the {len(failures)} failed tests are known to be flaky: each has "
            "failed in one attempt of a job and passed in another. If they are the job's only "
            "failures, re-run the failed jobs."
        )
    else:
        lines.append(f"None of the {len(failures)} failed tests is known to be flaky.")
    lines += [
        "",
        "| Test | Result | Known flaky |",
        "| --- | --- | --- |",
        *rows[:MAX_SUMMARY_ROWS],
    ]
    if len(rows) > MAX_SUMMARY_ROWS:
        lines += [
            "",
            f"The table shows {MAX_SUMMARY_ROWS} of the {len(rows)} failed tests.",
        ]
    summary.write("\n".join(lines) + "\n")


# ---------------------------------------------------------------------------------------------
# record
# ---------------------------------------------------------------------------------------------


def runs_to_record(
    gh: Any, run_id: int | None, sweep_days: int | None, now: datetime
) -> list[dict[str, Any]]:
    if run_id is not None:
        run = gh.get(f"repos/{gh.repo}/actions/runs/{run_id}")
        if run.get("path") != TESTS_WORKFLOW:
            raise SystemExit(f"Run {run_id} is not a run of {TESTS_WORKFLOW}.")
        return [run]
    assert sweep_days is not None
    since = (now - timedelta(days=sweep_days)).date().isoformat()
    path = (
        f"repos/{gh.repo}/actions/workflows/{TESTS_WORKFLOW.rsplit('/', 1)[1]}/runs"
        f"?status=completed&created=%3E%3D{since}&per_page=100"
    )
    # Only a re-run can have jobs with more than one attempt.
    return [
        run
        for run in gh.paginate(path, "workflow_runs")
        if run.get("run_attempt", 1) > 1
    ]


@dataclasses.dataclass
class Recorder:
    """Records the flaky failures of Tests runs in issues."""

    gh: Any
    # The default branch's checkout, which says which packages exist.
    repo_root: Path
    now: datetime
    log: Callable[[str], None]
    # Only logs the changes to issues.
    dry_run: bool = False

    def record(self, runs: Iterable[dict[str, Any]]) -> None:
        issues: dict[str, FlakyIssue] = {}
        dashboard = None
        for issue in list_issues(self.gh, "all"):
            if DASHBOARD_MARKER in (issue.get("body") or ""):
                dashboard = dashboard or issue
                continue
            flaky = parse_issue(issue)
            if flaky is not None:
                issues.setdefault(flaky.target, flaky)

        for run in runs:
            try:
                occurrences = self.find_flaky_failures(run)
            except OSError as error:
                # The sweeps try the run again until two days after it was created.
                self.log(f"Skipping run {run['id']}: {error}")
                continue
            opened = 0
            for occurrence in occurrences:
                flaky = issues.get(occurrence.target)
                if flaky is None:
                    if opened == MAX_NEW_ISSUES_PER_RUN:
                        self.log(
                            f"Skipping {occurrence.label}: run {run['id']} has too many "
                            "new flaky tests."
                        )
                        continue
                    opened += 1
                    flaky = issues[occurrence.target] = FlakyIssue(occurrence.target)
                if flaky.add(occurrence):
                    self.log(
                        f"{occurrence.label} failed ({occurrence.status}) in attempt "
                        f"{occurrence.attempt} of {occurrence.job} in run "
                        f"{occurrence.run_id}, and passed in another attempt."
                    )

        if not self.dry_run and any(flaky.number is None for flaky in issues.values()):
            self.ensure_label()
        self.save_issues(issues.values())
        self.save_dashboard(dashboard, issues.values())

    def find_flaky_failures(self, run: dict[str, Any]) -> list[Occurrence]:
        """Compares the attempts of each re-run test job of a Tests run."""
        gh = self.gh
        attempts: defaultdict[str, dict[int, dict[str, Any]]] = defaultdict(dict)
        path = f"repos/{gh.repo}/actions/runs/{run['id']}/artifacts?per_page=100"
        for artifact in gh.paginate(path, "artifacts"):
            match = ARTIFACT_RE.match(artifact.get("name", ""))
            if match and not artifact.get("expired"):
                attempts[match[1]][int(match[2])] = artifact

        pull_requests = run.get("pull_requests") or []
        found = []
        for job, artifacts in sorted(attempts.items()):
            if len(artifacts) < 2:
                continue
            results = {}
            for attempt, artifact in artifacts.items():
                try:
                    data = gh.download(
                        f"repos/{gh.repo}/actions/artifacts/{artifact['id']}/zip"
                    )
                    results[attempt] = parse_results_zip(data)
                except UNUSABLE_ARTIFACT_ERRORS as error:
                    self.log(
                        f"Skipping {artifact.get('name')} of run {run['id']}: {error}"
                    )
            for label, status, attempt in flaky_failures(results):
                if not package_exists(self.repo_root, label):
                    self.log(
                        f"Skipping {label} from run {run['id']}: its package doesn't "
                        "exist."
                    )
                    continue
                found.append(
                    Occurrence(
                        label=label,
                        job=job,
                        status=status,
                        run_id=int(run["id"]),
                        attempt=attempt,
                        sha=str(run["head_sha"]),
                        time=artifacts[attempt]["created_at"],
                        event=str(run["event"]),
                        pull_request=int(pull_requests[0]["number"])
                        if pull_requests
                        else None,
                    )
                )
        return found

    def ensure_label(self) -> None:
        gh = self.gh
        labels = gh.paginate(f"repos/{gh.repo}/labels?per_page=100")
        if not any(label["name"] == ISSUE_LABEL for label in labels):
            gh.post(
                f"repos/{gh.repo}/labels",
                {
                    "name": ISSUE_LABEL,
                    "color": "fbca04",
                    "description": "A test that failed and passed in attempts of a CI "
                    "job (flaky-tests.yml)",
                },
            )

    def save_issues(self, issues: Iterable[FlakyIssue]) -> None:
        gh = self.gh
        for flaky in sorted(issues, key=lambda f: f.target):
            if not flaky.changed:
                continue
            body = render_issue(flaky, gh.repo)
            if flaky.number is None:
                self.log(f"Opening an issue for {flaky.target}.")
                if not self.dry_run:
                    created = gh.post(
                        f"repos/{gh.repo}/issues",
                        {
                            "title": f"Flaky test: {flaky.target}",
                            "body": body,
                            "labels": [ISSUE_LABEL],
                        },
                    )
                    flaky.number = created["number"]
                continue
            update: dict[str, Any] = {"body": body}
            if flaky.reopened:
                update["state"] = "open"
                self.log(f"Reopening #{flaky.number} for {flaky.target}.")
            else:
                self.log(f"Updating #{flaky.number} for {flaky.target}.")
            if self.dry_run:
                continue
            gh.patch(f"repos/{gh.repo}/issues/{flaky.number}", update)
            if flaky.reopened:
                latest = flaky.occurrences[0]
                gh.post(
                    f"repos/{gh.repo}/issues/{flaky.number}/comments",
                    {
                        "body": f"Reopened: `{latest.label}` failed again in "
                        f"`{latest.job}` and passed in another attempt of the job "
                        f"(https://github.com/{gh.repo}/actions/runs/{latest.run_id}/"
                        f"attempts/{latest.attempt})."
                    },
                )

    def save_dashboard(
        self, dashboard: dict[str, Any] | None, issues: Iterable[FlakyIssue]
    ) -> None:
        gh = self.gh
        issues = list(issues)
        body = render_dashboard(issues, gh.repo, self.now)
        if dashboard is None:
            if not issues:
                return
            self.log("Opening the dashboard issue.")
            if not self.dry_run:
                gh.post(
                    f"repos/{gh.repo}/issues",
                    {"title": DASHBOARD_TITLE, "body": body, "labels": [ISSUE_LABEL]},
                )
            return
        # A description edited in GitHub's UI has Windows line endings.
        current = (dashboard.get("body") or "").replace("\r\n", "\n")
        if current.strip() == body.strip():
            return
        self.log(f"Updating the dashboard, #{dashboard['number']}.")
        if not self.dry_run:
            gh.patch(f"repos/{gh.repo}/issues/{dashboard['number']}", {"body": body})


# ---------------------------------------------------------------------------------------------


def github_from_env(repo: str) -> GitHub:
    token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
    if not token:
        raise SystemExit("Set GH_TOKEN to a GitHub token.")
    return GitHub(repo, token)


def main(argv: list[str] | None = None) -> None:
    parser = argparse.ArgumentParser(description="Tracks flaky Bazel tests in CI.")
    commands = parser.add_subparsers(dest="command", required=True)

    extract_parser = commands.add_parser(
        "extract", help="reduce a build event file to results"
    )
    extract_parser.add_argument("bep_file", type=Path)
    extract_parser.add_argument("output", type=Path)

    summarize_parser = commands.add_parser(
        "summarize", help="note known flaky test failures"
    )
    summarize_parser.add_argument("results", type=Path)
    summarize_parser.add_argument("--repo", required=True)

    record_parser = commands.add_parser("record", help="record flaky tests as issues")
    record_parser.add_argument("--repo", required=True)
    runs = record_parser.add_mutually_exclusive_group(required=True)
    runs.add_argument("--run-id", type=int, help="the Tests run to record")
    runs.add_argument(
        "--sweep-days", type=int, help="record the re-runs of the last N days"
    )
    record_parser.add_argument("--repo-root", type=Path, default=Path())
    record_parser.add_argument(
        "--dry-run", action="store_true", help="change no issues"
    )

    args = parser.parse_args(argv)
    if args.command == "extract":
        extract(args.bep_file, args.output)
    elif args.command == "summarize":
        summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
        gh = github_from_env(args.repo)
        if summary_path:
            with open(summary_path, "a", encoding="utf-8") as summary:
                summarize(args.results, gh, summary, sys.stdout)
        else:
            summarize(args.results, gh, sys.stdout, sys.stdout)
    else:
        gh = github_from_env(args.repo)
        now = datetime.now(UTC)
        logged = []

        def log(message: str) -> None:
            print(message)
            logged.append(message)

        runs_list = runs_to_record(gh, args.run_id, args.sweep_days, now)
        log(
            f"Recording {len(runs_list)} Tests runs{' (dry run)' if args.dry_run else ''}."
        )
        Recorder(gh, args.repo_root, now, log, dry_run=args.dry_run).record(runs_list)
        summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
        if summary_path:
            with open(summary_path, "a", encoding="utf-8") as summary:
                summary.write("".join(f"- {message}\n" for message in logged))


if __name__ == "__main__":
    main()
