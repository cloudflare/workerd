# WPT results on wpt.fyi

[wpt.fyi](https://wpt.fyi) is the dashboard of [Web Platform Tests](https://web-platform-tests.org)
results. Build & Release (`.github/workflows/release.yml`) submits the WPT results of each workerd
release to it, as the product `workerd`, so that they can be compared with browsers' and other
runtimes' results. Node.js submits its results the same way.

## What is submitted

Every release runs the WPT suites under `src/wpt` on each platform. Once the release is published,
the `wpt-fyi` job (`.github/workflows/_wpt-fyi.yml`) converts the Linux X64 results into a
wptreport file, the format of `wpt run --log-wptreport`, with `bazel run //tools:wpt_fyi_report`
(`tools/cross/wpt_fyi_report.py`), and submits it to wpt.fyi's [results upload API][upload-api].
Daily releases make this a daily run. Prereleases are not submitted.

The run is described as follows:

| Field             | Value                                                                       |
| ----------------- | --------------------------------------------------------------------------- |
| `product`         | `workerd`                                                                   |
| `browser_version` | The release version, like `1.20261023.1`                                    |
| `browser_channel` | `stable`                                                                    |
| `os`              | `linux`                                                                     |
| `revision`        | The WPT commit that the release pins in `build/deps/shared_deps.jsonc`      |
| labels            | `master`, since that commit is on WPT's master branch, and the channel name |

Results are the outcomes of the test files that workerd runs, not workerd's expectations: an
expected failure is reported as `FAIL`. Subtests that a test config disables or omits are `NOTRUN`,
and files that it disables or omits as a whole are left out, as are tests that workerd does not run
at all, which wpt.fyi shows as missing.

Some WPT directories are tested by more than one suite, under different compatibility flags. Only
one of them is reported: the suites marked `wpt_fyi = False` in `src/wpt/BUILD.bazel` are left
out, which are `urlpattern` in favour of `urlpattern-standard`, and `fetch/api`, `compression`,
`streams` and `encoding` in favour of their `-ts` suites. The report generator fails if two
reported suites give the same test.

### How results map to WPT's tests

workerd's harness does not run tests the way WPT does, so the results on wpt.fyi are an
approximation. WPT runs each test URL separately: each global that a file's `// META: global=`
asks for, and each of its `// META: variant=` query strings, is a test of its own on wpt.fyi.
workerd runs each file once, in workerd's own global scope, which is none of WPT's, without a
query string, and runs every subtest. The report then:

- labels that run as one of WPT's globals: a multi-global test (`.any.js`) is reported as its
  dedicated worker test, `.any.worker.html`, or if the file does not ask for that global, the
  first of window, service worker and shared worker that it asks for;
- makes up a result for each variant from the subtests that the variant's query string would have
  selected through `subsetTest()` or `subsetTestByKey()`, and reports a file whose variants select
  a configuration, like the websockets tests' `?default`, `?wss` and `?wpt_flags=h2`, as its
  `?default` variant;
- leaves out files whose variants choose subtests some other way, which cannot be split.

So a file's rows for different globals or variants on wpt.fyi all come from that one run; they are
not separate runs.

`tools/cross/wpt_fyi_report.py` derives the test IDs from each file's path and META comments,
following WPT's rules in `tools/manifest/sourcefile.py`: `x.any.js` gives `x.any.html`,
`x.any.worker.html` and so on for the globals that its `// META: global=` names, `x.window.js` gives
`x.window.html` and `x.worker.js` gives `x.worker.html`, and each `// META: variant=` appends its
query string. The harness records the META comments in its report for this. If the harness ran
each test URL the way WPT defines it, each variant with its query string, under a deliberately
chosen global, the IDs could instead come straight from the `MANIFEST.json` that WPT publishes,
which would be more faithful.

### The WPT revision

wpt.fyi files a run under the full hash of the WPT commit it tested. The WPT release that
`build/deps/shared_deps.jsonc` pins is named after an abbreviated hash, so the report takes the full
one from the `REVISION` file at the root of the release, which the cloudflare/workerd-tools WPT
workflow (`.github/workflows/wpt.yml`) adds. Releases made before that workflow added it,
including `wpt-2f17dec1f`, do not have the file, and `bazel run //tools:wpt_fyi_report` fails to
build for them, on the missing `@wpt//:REVISION`. Reporting therefore needs these steps, in order:

1. The workerd-tools change that adds `REVISION` to the release archive is merged.
2. A WPT release is cut with that workflow, by its weekly schedule or by running it by hand.
3. workerd pins that release with `build/deps/update_wpt.py <tag>`, and updates the test configs
   for the new WPT version as usual.

Until then, the `wpt-fyi` job of Build & Release fails without submitting anything.

wpt.fyi ignores a report whose results are identical to one it already has, so submitting a
release twice does not create a second run.

## Turning on submission

Submission needs an uploader account on wpt.fyi. Until the secrets below are set, the job still
generates the report and uploads it as the `wpt-fyi-report-<version>` artifact of the run, logs
that it did not submit it, and succeeds.

1. Ask the wpt.fyi maintainers to add `workerd` as a product and to register an uploader, by
   [filing an issue][new-issue] like [Node.js did][node-issue]. Adding a product is a change to
   `shared/browsers.go` and the product logos in wpt.fyi, like the one for
   [Deno][deno-product]. The maintainers may hand out staging credentials first.
2. Set the repository secrets `WPT_FYI_USERNAME` and `WPT_FYI_PASSWORD` to the uploader's
   credentials.
3. To submit to `https://staging.wpt.fyi` rather than `https://wpt.fyi`, set the repository
   variable `WPT_FYI_URL` to `https://staging.wpt.fyi`. Remove it to submit to production.

## Submitting a past release

The "Submit WPT results to wpt.fyi" workflow (`.github/workflows/wpt-fyi.yml`) generates the
report of a given release from the test logs of its Build & Release run, with that release's tools,
and submits it unless `dry_run` is set, which it is by default. It needs the run's test log
artifact, which expires (after 90 days by default), and a release whose WPT harness predates
wpt.fyi support cannot be reported.

## Checking the report

To check the report without submitting it, run the "Submit WPT results to wpt.fyi" workflow for a
release with `dry_run` on (the default). It uploads the report as an artifact, and its step summary
lists how many tests and subtests it reports, and what it left out and why. To generate a report
locally:

```sh
bazel test --config=wpt-test //src/wpt/...
bazel run //tools:wpt_fyi_report -- --output report.json bazel-testlogs/
```

An `--output` name that does not end in `.gz` is written uncompressed. To try this with a pinned
WPT release that has no `REVISION` file, pass Bazel `--override_repository=wpt=<dir>`, where `<dir>`
is the extracted release with a `REVISION` file added. See
`bazel run //tools:wpt_fyi_report -- --help` for the other options.

[upload-api]: https://github.com/web-platform-tests/wpt.fyi/blob/main/api/README.md#apiresultsupload
[new-issue]: https://github.com/web-platform-tests/wpt.fyi/issues/new
[node-issue]: https://github.com/web-platform-tests/wpt.fyi/issues/3144
[deno-product]: https://github.com/web-platform-tests/wpt.fyi/pull/2553
