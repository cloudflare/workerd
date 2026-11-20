"""Tests for flaky_tests.py.

GitHub is faked in memory: FakeGitHub serves the runs, artifacts, issues and labels a test sets up,
and records every write.
"""

import dataclasses
import io
import json
import re
import tempfile
import unittest
import urllib.error
import zipfile
from datetime import UTC, datetime
from pathlib import Path

import flaky_tests

REPO = "owner/repo"
NOW = datetime(2026, 11, 20, 12, 0, tzinfo=UTC)


def bep_event(label, status):
    return json.dumps(
        {
            "id": {"testSummary": {"label": label, "configuration": {"id": "c55f"}}},
            "testSummary": {
                "totalRunCount": 1,
                "overallStatus": status,
                "attemptCount": 1,
            },
        }
    )


def results_zip(tests, version=flaky_tests.RESULTS_VERSION):
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w") as archive:
        archive.writestr(
            flaky_tests.RESULTS_FILE, json.dumps({"version": version, "tests": tests})
        )
    return buffer.getvalue()


class FakeGitHub:
    def __init__(self):
        self.repo = REPO
        self.runs = {}
        self.artifacts = {}
        self.blobs = {}
        self.issues = {}
        self.labels = [{"name": "bug"}]
        self.writes = []
        self.downloads = []
        self.fail_reads = False
        # Artifact IDs whose downloads, and run IDs whose artifact listings, fail.
        self.failing_downloads = set()
        self.failing_listings = set()

    def add_run(self, run_id, event="pull_request", pull_request=8700, attempts=2):
        self.runs[run_id] = {
            "id": run_id,
            "path": flaky_tests.TESTS_WORKFLOW,
            "run_attempt": attempts,
            "head_sha": "0123456789abcdef0123456789abcdef01234567",
            "event": event,
            "pull_requests": [{"number": pull_request}] if pull_request else [],
        }
        self.artifacts.setdefault(run_id, [])
        return self.runs[run_id]

    def add_results(
        self, run_id, job, attempt, tests, created_at="2026-11-19T10:00:00Z"
    ):
        artifact_id = len(self.blobs) + 1
        self.blobs[artifact_id] = (
            results_zip(tests) if isinstance(tests, dict) else tests
        )
        self.artifacts[run_id].append(
            {
                "id": artifact_id,
                "name": f"test-results-{job}-attempt{attempt}",
                "created_at": created_at,
                "expired": False,
            }
        )

    def add_issue(self, body, state="open", closed_at=None, title="Flaky test"):
        number = 100 + len(self.issues)
        self.issues[number] = {
            "number": number,
            "title": title,
            "body": body,
            "state": state,
            "closed_at": closed_at,
            "labels": [{"name": flaky_tests.ISSUE_LABEL}],
        }
        return number

    def get(self, path):
        match = re.fullmatch(rf"repos/{REPO}/actions/runs/(\d+)", path)
        assert match, path
        return self.runs[int(match[1])]

    def paginate(self, path, key=None):
        if self.fail_reads:
            raise OSError("network down")
        if match := re.fullmatch(
            rf"repos/{REPO}/actions/runs/(\d+)/artifacts\?.*", path
        ):
            assert key == "artifacts"
            if int(match[1]) in self.failing_listings:
                raise urllib.error.URLError("connection reset")
            return iter(self.artifacts[int(match[1])])
        if match := re.fullmatch(
            rf"repos/{REPO}/issues\?labels=flaky-test&state=(\w+)&.*", path
        ):
            state = match[1]
            return iter(
                [
                    issue
                    for issue in self.issues.values()
                    if state == "all" or issue["state"] == state
                ]
            )
        if re.fullmatch(rf"repos/{REPO}/labels\?.*", path):
            return iter(self.labels)
        if re.fullmatch(rf"repos/{REPO}/actions/workflows/test.yml/runs\?.*", path):
            assert key == "workflow_runs"
            return iter(self.runs.values())
        raise AssertionError(path)

    def download(self, path):
        match = re.fullmatch(rf"repos/{REPO}/actions/artifacts/(\d+)/zip", path)
        assert match, path
        self.downloads.append(int(match[1]))
        if int(match[1]) in self.failing_downloads:
            raise urllib.error.HTTPError(path, 404, "Not Found", None, None)
        return self.blobs[int(match[1])]

    def post(self, path, body):
        self.writes.append(("POST", path, body))
        if path == f"repos/{REPO}/issues":
            number = self.add_issue(body["body"], title=body["title"])
            return self.issues[number]
        if path == f"repos/{REPO}/labels":
            self.labels.append(body)
        return {}

    def patch(self, path, body):
        self.writes.append(("PATCH", path, body))
        number = int(path.rsplit("/", 1)[1])
        self.issues[number].update(body)
        if body.get("state") == "open":
            self.issues[number]["closed_at"] = None
        return self.issues[number]


class RepoRootTestCase(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        for package in ["src/workerd/api", "src/workerd/server/tests/cluster"]:
            (self.root / package).mkdir(parents=True)
            (self.root / package / "BUILD.bazel").write_text("")
        self.logged = []

    def record(self, gh, run_ids, dry_run=False):
        runs = [gh.runs[run_id] for run_id in run_ids]
        recorder = flaky_tests.Recorder(gh, self.root, NOW, self.logged.append, dry_run)
        recorder.record(runs)

    def issues_by_target(self, gh):
        return {
            flaky.target: flaky
            for flaky in map(flaky_tests.parse_issue, gh.issues.values())
            if flaky is not None
        }


class LabelTest(RepoRootTestCase):
    def test_split_label(self):
        self.assertEqual(
            flaky_tests.split_label("//src/workerd/api:streams-test@"),
            ("//src/workerd/api:streams-test", "@"),
        )
        self.assertEqual(
            flaky_tests.split_label(
                "//src/workerd/api:pyodide/pyodide-test@all-autogates"
            ),
            ("//src/workerd/api:pyodide/pyodide-test", "@all-autogates"),
        )
        self.assertEqual(
            flaky_tests.split_label("//src/workerd/server:workerd-cli_test"),
            ("//src/workerd/server:workerd-cli_test", ""),
        )
        self.assertEqual(
            flaky_tests.split_label("//:top@all-compat-flags"),
            ("//:top", "@all-compat-flags"),
        )

    def test_rejects_labels_that_could_inject_markdown(self):
        for label in [
            "//a:b <img>",
            "//a:b`c",
            "@other//a:b",
            "//a:b\n",
            "//a:b|c",
            "//a:b@x@y",
        ]:
            with self.subTest(label=label):
                self.assertRaises(ValueError, flaky_tests.split_label, label)

    def test_package_exists(self):
        self.assertTrue(
            flaky_tests.package_exists(self.root, "//src/workerd/api:streams-test@")
        )
        self.assertFalse(
            flaky_tests.package_exists(self.root, "//src/workerd/io:io-gate-test@")
        )
        self.assertFalse(
            flaky_tests.package_exists(self.root, "//src/workerd/api/..:x@")
        )
        self.assertFalse(flaky_tests.package_exists(self.root, "not a label"))


class ExtractTest(unittest.TestCase):
    def test_reads_each_tests_overall_status(self):
        lines = [
            json.dumps({"id": {"started": {}}, "started": {}}),
            # Console output that mentions a test summary.
            json.dumps(
                {
                    "id": {"progress": {"opaqueCount": 1}},
                    "progress": {
                        "stdout": '{"testSummary": {"overallStatus": "FAILED"}}'
                    },
                }
            ),
            bep_event("//src/workerd/api:a-test@", "PASSED"),
            bep_event("//src/workerd/api:b-test@", "TIMEOUT"),
            # A test in two configurations keeps its failure.
            bep_event("//src/workerd/api:c-test@", "FAILED"),
            bep_event("//src/workerd/api:c-test@", "PASSED"),
            bep_event("//src/workerd/api:d-test@", "FLAKY"),
            # A job stopped while Bazel wrote the file.
            bep_event("//src/workerd/api:e-test@", "PASSED")[:40],
        ]
        self.assertEqual(
            flaky_tests.read_test_statuses(lines),
            {
                "//src/workerd/api:a-test@": "PASSED",
                "//src/workerd/api:b-test@": "TIMEOUT",
                "//src/workerd/api:c-test@": "FAILED",
                "//src/workerd/api:d-test@": "FLAKY",
            },
        )

    def test_writes_results_that_parse_results_accepts(self):
        with tempfile.TemporaryDirectory() as directory:
            bep = Path(directory, "test.bep.json")
            bep.write_text(bep_event("//src/workerd/api:a-test@", "FAILED") + "\n")
            output = Path(directory, flaky_tests.RESULTS_FILE)
            flaky_tests.extract(bep, output)
            parsed = flaky_tests.parse_results(json.loads(output.read_text()))
        self.assertEqual(parsed, {"//src/workerd/api:a-test@": "FAILED"})

    def test_writes_nothing_without_a_build_event_file(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory, flaky_tests.RESULTS_FILE)
            flaky_tests.extract(Path(directory, "missing.json"), output)
            self.assertFalse(output.exists())


class ParseResultsTest(unittest.TestCase):
    def test_drops_invalid_tests(self):
        parsed = flaky_tests.parse_results(
            {
                "version": flaky_tests.RESULTS_VERSION,
                "tests": {
                    "//src/workerd/api:a-test@": "FAILED",
                    "//src/workerd/api:b-test@": "EXPLODED",
                    "//src/workerd/api:c test": "FAILED",
                    "//src/workerd/api:d-test@": ["FAILED"],
                },
            }
        )
        self.assertEqual(parsed, {"//src/workerd/api:a-test@": "FAILED"})

    def test_rejects_unknown_formats(self):
        for data in [[], {"version": 2, "tests": {}}, {"version": 1, "tests": []}]:
            with self.subTest(data=data):
                self.assertRaises(
                    (TypeError, ValueError), flaky_tests.parse_results, data
                )

    def test_zip_must_hold_the_results_file(self):
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, "w") as archive:
            archive.writestr("other.json", "{}")
        self.assertRaises(KeyError, flaky_tests.parse_results_zip, buffer.getvalue())


class FlakyFailuresTest(unittest.TestCase):
    def test_failure_that_another_attempt_passed(self):
        results = {
            1: {
                "//a:flaky@": "FAILED",
                "//a:timeout@": "TIMEOUT",
                "//a:broken@": "FAILED",
                "//a:ok@": "PASSED",
            },
            2: {
                "//a:flaky@": "PASSED",
                "//a:timeout@": "FLAKY",
                "//a:broken@": "FAILED",
                "//a:ok@": "PASSED",
            },
        }
        self.assertEqual(
            flaky_tests.flaky_failures(results),
            [("//a:flaky@", "FAILED", 1), ("//a:timeout@", "TIMEOUT", 1)],
        )

    def test_a_pass_before_the_failure_counts(self):
        # A re-run that misses the cache runs tests that passed in the earlier attempt again.
        results = {1: {"//a:t@": "PASSED"}, 2: {"//a:t@": "FAILED"}}
        self.assertEqual(flaky_tests.flaky_failures(results), [("//a:t@", "FAILED", 2)])

    def test_every_failure_counts(self):
        results = {
            1: {"//a:t@": "FAILED"},
            2: {"//a:t@": "TIMEOUT"},
            3: {"//a:t@": "PASSED"},
        }
        self.assertEqual(
            flaky_tests.flaky_failures(results),
            [("//a:t@", "FAILED", 1), ("//a:t@", "TIMEOUT", 2)],
        )

    def test_a_test_missing_from_the_later_attempt_did_not_pass(self):
        results = {1: {"//a:t@": "FAILED"}, 2: {"//a:t@": "NO_STATUS"}, 3: {}}
        self.assertEqual(flaky_tests.flaky_failures(results), [])


class RecordTest(RepoRootTestCase):
    def flaky_run(
        self,
        gh,
        run_id=1,
        label="//src/workerd/api:streams-test@",
        job="windows-X64",
        **run,
    ):
        gh.add_run(run_id, **run)
        gh.add_results(
            run_id, job, 1, {label: "TIMEOUT", "//src/workerd/api:ok-test@": "PASSED"}
        )
        gh.add_results(
            run_id,
            job,
            2,
            {label: "PASSED", "//src/workerd/api:ok-test@": "PASSED"},
            created_at="2026-11-19T11:00:00Z",
        )

    def test_opens_an_issue_and_the_dashboard(self):
        gh = FakeGitHub()
        self.flaky_run(gh)
        self.record(gh, [1])

        issue = next(
            i for i in gh.issues.values() if i["title"].startswith("Flaky test: ")
        )
        self.assertEqual(issue["title"], "Flaky test: //src/workerd/api:streams-test")
        self.assertIn(
            "| 2026-11-19 10:00 | `windows-X64` | `@` | TIMEOUT |", issue["body"]
        )
        self.assertIn("/actions/runs/1/attempts/1", issue["body"])
        self.assertIn("| PR 8700 |", issue["body"])
        self.assertNotIn("#8700", issue["body"])
        flaky = self.issues_by_target(gh)["//src/workerd/api:streams-test"]
        self.assertEqual(flaky.total, 1)
        self.assertEqual(flaky.occurrences[0].label, "//src/workerd/api:streams-test@")

        dashboard = next(
            i for i in gh.issues.values() if i["title"] == flaky_tests.DASHBOARD_TITLE
        )
        self.assertIn(
            f"| `//src/workerd/api:streams-test` | #{issue['number']} | 1 | 1 | `windows-X64` (1) |",
            dashboard["body"],
        )
        self.assertIn(flaky_tests.ISSUE_LABEL, [label["name"] for label in gh.labels])

    def test_recording_a_run_again_changes_nothing(self):
        gh = FakeGitHub()
        self.flaky_run(gh)
        self.record(gh, [1])
        writes = len(gh.writes)
        self.record(gh, [1])
        self.assertEqual(len(gh.writes), writes)

    def test_groups_variants_into_one_issue(self):
        gh = FakeGitHub()
        self.flaky_run(gh, 1, "//src/workerd/api:streams-test@")
        self.flaky_run(
            gh, 2, "//src/workerd/api:streams-test@all-autogates", job="macOS-ARM64"
        )
        self.record(gh, [1, 2])
        flaky = self.issues_by_target(gh)["//src/workerd/api:streams-test"]
        self.assertEqual(flaky.total, 2)
        self.assertEqual(
            sorted(o.variant for o in flaky.occurrences), ["@", "@all-autogates"]
        )
        self.assertEqual(
            sum(i["title"].startswith("Flaky test: ") for i in gh.issues.values()), 1
        )

    def test_adds_to_an_existing_issue(self):
        gh = FakeGitHub()
        self.flaky_run(gh, 1)
        self.record(gh, [1])
        self.flaky_run(gh, 2, event="push", pull_request=None)
        self.record(gh, [2])
        flaky = self.issues_by_target(gh)["//src/workerd/api:streams-test"]
        self.assertEqual(flaky.total, 2)
        self.assertEqual([o.run_id for o in flaky.occurrences], [2, 1])
        self.assertIn("| main |", gh.issues[flaky.number]["body"])

    def test_reopens_a_closed_issue_that_fails_again(self):
        gh = FakeGitHub()
        self.flaky_run(gh, 1)
        self.record(gh, [1])
        number = self.issues_by_target(gh)["//src/workerd/api:streams-test"].number
        gh.issues[number].update(state="closed", closed_at="2026-11-19T10:30:00Z")

        # A failure before the issue was closed doesn't reopen it.
        gh.add_run(2)
        gh.add_results(
            2,
            "linux-X64",
            1,
            {"//src/workerd/api:streams-test@": "FAILED"},
            "2026-11-19T10:10:00Z",
        )
        gh.add_results(
            2,
            "linux-X64",
            2,
            {"//src/workerd/api:streams-test@": "PASSED"},
            "2026-11-19T10:20:00Z",
        )
        self.record(gh, [2])
        self.assertEqual(gh.issues[number]["state"], "closed")

        self.flaky_run(gh, 3)
        gh.artifacts[3][0]["created_at"] = "2026-11-19T11:30:00Z"
        self.record(gh, [3])
        self.assertEqual(gh.issues[number]["state"], "open")
        comments = [body for _, path, body in gh.writes if path.endswith("/comments")]
        self.assertEqual(len(comments), 1)
        self.assertIn("/actions/runs/3/attempts/1", comments[0]["body"])

    def test_ignores_a_job_that_ran_once(self):
        gh = FakeGitHub()
        gh.add_run(1)
        gh.add_results(
            1, "windows-X64", 1, {"//src/workerd/api:streams-test@": "FAILED"}
        )
        gh.add_results(
            1, "macOS-ARM64", 2, {"//src/workerd/api:streams-test@": "PASSED"}
        )
        self.record(gh, [1])
        self.assertEqual(gh.downloads, [])
        self.assertEqual(gh.writes, [])

    def test_skips_tests_in_packages_that_dont_exist(self):
        gh = FakeGitHub()
        self.flaky_run(gh, label="//src/made/up:spam-test@")
        self.record(gh, [1])
        self.assertEqual(gh.writes, [])

    def test_skips_unreadable_artifacts(self):
        gh = FakeGitHub()
        self.flaky_run(gh)
        gh.add_results(1, "linux-X64", 1, b"not a zip")
        gh.add_results(1, "linux-X64", 2, results_zip({}, version=9))
        self.record(gh, [1])
        self.assertEqual(
            list(self.issues_by_target(gh)), ["//src/workerd/api:streams-test"]
        )
        self.assertTrue(any("linux-X64-attempt1" in message for message in self.logged))

    def test_skips_an_artifact_that_fails_to_download(self):
        gh = FakeGitHub()
        self.flaky_run(gh)
        gh.add_results(1, "linux-X64", 1, {"//src/workerd/api:other-test@": "FAILED"})
        gh.add_results(1, "linux-X64", 2, {"//src/workerd/api:other-test@": "PASSED"})
        # The artifact expired after the listing.
        gh.failing_downloads.add(gh.artifacts[1][-2]["id"])
        self.record(gh, [1])
        self.assertEqual(
            list(self.issues_by_target(gh)), ["//src/workerd/api:streams-test"]
        )
        self.assertTrue(
            any(
                "linux-X64-attempt1" in message and "404" in message
                for message in self.logged
            )
        )

    def test_skips_a_run_whose_artifacts_cant_be_listed(self):
        gh = FakeGitHub()
        self.flaky_run(gh, 1, "//src/workerd/api:streams-test@")
        self.flaky_run(gh, 2, "//src/workerd/api:other-test@")
        gh.failing_listings.add(1)
        self.record(gh, [1, 2])
        self.assertEqual(
            list(self.issues_by_target(gh)), ["//src/workerd/api:other-test"]
        )
        self.assertTrue(any(m.startswith("Skipping run 1: ") for m in self.logged))

    def test_limits_the_issues_one_run_opens(self):
        gh = FakeGitHub()
        gh.add_run(1)
        labels = [
            f"//src/workerd/api:test-{n}@"
            for n in range(flaky_tests.MAX_NEW_ISSUES_PER_RUN + 5)
        ]
        gh.add_results(1, "linux-X64", 1, {label: "FAILED" for label in labels})
        gh.add_results(1, "linux-X64", 2, {label: "PASSED" for label in labels})
        self.record(gh, [1])
        self.assertEqual(
            len(self.issues_by_target(gh)), flaky_tests.MAX_NEW_ISSUES_PER_RUN
        )

    def test_dry_run_changes_nothing(self):
        gh = FakeGitHub()
        self.flaky_run(gh)
        self.record(gh, [1], dry_run=True)
        self.assertEqual(gh.writes, [])
        self.assertIn(
            "Opening an issue for //src/workerd/api:streams-test.", self.logged
        )

    def test_issue_data_cant_end_its_comment(self):
        flaky = flaky_tests.FlakyIssue("//src/workerd/api:streams-test")
        flaky.add(
            flaky_tests.Occurrence(
                label="//src/workerd/api:streams-test@",
                job="linux-X64",
                status="FAILED",
                run_id=1,
                attempt=1,
                sha="-->",
                time="2026-11-19T10:00:00Z",
                event="pull_request",
                pull_request=None,
            )
        )
        self.assertEqual(flaky_tests.data_block(flaky).count("-->"), 1)
        body = flaky_tests.render_issue(flaky, REPO)
        parsed = flaky_tests.parse_issue({"number": 1, "state": "open", "body": body})
        self.assertEqual(parsed.occurrences, flaky.occurrences)

    def test_runs_to_record(self):
        gh = FakeGitHub()
        gh.add_run(1, attempts=1)
        gh.add_run(2, attempts=2)
        self.assertEqual(
            [r["id"] for r in flaky_tests.runs_to_record(gh, None, 2, NOW)], [2]
        )
        self.assertEqual(
            [r["id"] for r in flaky_tests.runs_to_record(gh, 1, None, NOW)], [1]
        )
        gh.runs[1]["path"] = ".github/workflows/release.yml"
        self.assertRaises(SystemExit, flaky_tests.runs_to_record, gh, 1, None, NOW)


class DashboardTest(unittest.TestCase):
    def occurrence(self, label, job, day):
        return flaky_tests.Occurrence(
            label=label,
            job=job,
            status="FAILED",
            run_id=day,
            attempt=1,
            sha="0" * 40,
            time=f"2026-11-{day:02}T10:00:00Z",
            event="push",
            pull_request=None,
        )

    def test_ranks_open_issues_by_recent_failures(self):
        rare = flaky_tests.FlakyIssue("//a:rare", number=1)
        rare.add(self.occurrence("//a:rare@", "linux-X64", 19))
        common = flaky_tests.FlakyIssue("//a:common", number=2)
        for day, job in [(19, "windows-X64"), (18, "windows-X64"), (10, "macOS-ARM64")]:
            common.add(self.occurrence("//a:common@", job, day))
        # Failures more than 30 days old don't count.
        common.add(
            dataclasses.replace(
                self.occurrence("//a:common@", "linux-X64", 1),
                time="2026-10-01T00:00:00Z",
            )
        )
        closed = flaky_tests.FlakyIssue(
            "//a:closed", number=3, closed_at="2026-11-19T12:00:00Z"
        )
        closed.add(self.occurrence("//a:closed@", "linux-X64", 18))

        body = flaky_tests.render_dashboard([rare, common, closed], REPO, NOW)
        rows = [line for line in body.splitlines() if line.startswith("| `//")]
        self.assertEqual(
            rows,
            [
                "| `//a:common` | #2 | 2 | 3 | `windows-X64` (2), `macOS-ARM64` (1) | 2026-11-19 |",
                "| `//a:rare` | #1 | 1 | 1 | `linux-X64` (1) | 2026-11-19 |",
            ],
        )
        self.assertIn("| `windows-X64` | 2 | 1 |", body)
        self.assertNotIn("//a:closed", body)

    def test_says_when_nothing_failed(self):
        body = flaky_tests.render_dashboard([], REPO, NOW)
        self.assertIn("No test has failed this way in the last 30 days.", body)


class SummarizeTest(RepoRootTestCase):
    def write_results(self, tests):
        path = self.root / flaky_tests.RESULTS_FILE
        path.write_text(
            json.dumps({"version": flaky_tests.RESULTS_VERSION, "tests": tests})
        )
        return path

    def summarize(self, gh, tests):
        summary, log = io.StringIO(), io.StringIO()
        flaky_tests.summarize(self.write_results(tests), gh, summary, log)
        return summary.getvalue(), log.getvalue()

    def test_notes_known_flaky_tests(self):
        gh = FakeGitHub()
        number = gh.add_issue(
            "<!-- flaky-test: //src/workerd/api:streams-test -->\n..."
        )
        gh.add_issue(
            "<!-- flaky-test: //src/workerd/api:closed-test -->", state="closed"
        )
        summary, log = self.summarize(
            gh,
            {
                "//src/workerd/api:streams-test@all-autogates": "TIMEOUT",
                "//src/workerd/api:closed-test@": "FAILED",
                "//src/workerd/api:ok-test@": "PASSED",
            },
        )
        self.assertIn("1 of the 2 failed tests are known to be flaky", summary)
        self.assertIn(
            f"| `//src/workerd/api:streams-test@all-autogates` | TIMEOUT | "
            f"[#{number}](https://github.com/{REPO}/issues/{number}) |",
            summary,
        )
        self.assertIn("| `//src/workerd/api:closed-test@` | FAILED |  |", summary)
        self.assertEqual(log.count("::warning title=Known flaky test::"), 1)

    def test_lists_failures_when_the_lookup_fails(self):
        gh = FakeGitHub()
        gh.fail_reads = True
        summary, log = self.summarize(gh, {"//src/workerd/api:streams-test@": "FAILED"})
        self.assertIn(
            "Could not look up which of these tests are known to be flaky.", summary
        )
        self.assertIn("| `//src/workerd/api:streams-test@` | FAILED |  |", summary)
        self.assertIn("::warning::Could not look up", log)

    def test_says_nothing_without_failures(self):
        summary, log = self.summarize(
            FakeGitHub(), {"//src/workerd/api:ok-test@": "PASSED"}
        )
        self.assertEqual((summary, log), ("", ""))


class NextPageTest(unittest.TestCase):
    def test_next_page(self):
        link = '<https://api.github.com/x?page=2>; rel="next", <https://api.github.com/x?page=5>; rel="last"'
        self.assertEqual(flaky_tests.next_page(link), "https://api.github.com/x?page=2")
        self.assertIsNone(
            flaky_tests.next_page('<https://api.github.com/x?page=1>; rel="prev"')
        )
        self.assertIsNone(flaky_tests.next_page(None))


if __name__ == "__main__":
    unittest.main()
