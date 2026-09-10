---
name: wpt-update
description: >-
  Updates and triages the Web Platform Tests dependency in workerd. Use when
  asked to bump, update, or triage WPT.
---

# WPT Update

## Context

WPT (Web Platform Tests) is an open-source test suite for assessing runtime
conformance with Web standards. workerd keeps configuration under `src/wpt`
because some tests are not applicable or cover behavior workerd does not yet
implement.

A request to bump or update WPT includes both the dependency update and triage
unless the user explicitly requests a bump-only change.

## Scope

Allowed edits are dependency metadata produced by the updater and WPT config
files under `src/wpt`. Do not fix runtime conformance bugs during a WPT update.
If correct triage requires runtime, harness, build-rule, patch, or upstream
changes, report the required follow-up but don't make changes unless the user
expands the scope.

## Preflight

1. Determine whether workerd is standalone or a submodule and read all
   applicable `AGENTS.md` files.
2. Inspect the worktree and preserve unrelated changes.
3. Load `ts-style` before editing TypeScript config files.

## Bumping WPT

From the workerd root, run:

```bash
./build/deps/update_wpt.py
```

The updater selects the newest published `wpt-*` release from
`cloudflare/workerd-tools`. A normal update changes only:

* `build/deps/shared_deps.jsonc`
* `build/deps/gen/shared_deps.MODULE.bazel`

Verify that the new tag appears consistently in `freeze_version`,
`strip_prefix`, and the archive URL, and that a new SHA-256 was generated. Do
not hand-edit generated dependency output.

The updater writes the manifest before downloading and regenerating metadata.
If it fails, do not continue with inconsistent files. Resolve the failure,
rerun the updater, and verify both files.

The generic dependency updater may mention an unrelated available release
because `workerd-tools` contains several tools. Determine success from the
selected WPT tag and resulting diff.

If the updater reports that WPT is current, stop for a bump-only request. Keep
the dependency changes separate from triage changes. If commits were explicitly
requested, commit the dependency update before moving on. Otherwise, continue
without committing.

## Triaging a WPT update

For a triage-only request, first verify that a pin change exists relative to
the agreed base. Read the old and new `wpt-<sha>` values from the dependency
diff. The suffix identifies the corresponding upstream WPT commit.

Use pinned SHAs for all upstream research. Do not inspect files from upstream
`main`. GitHub's compare API truncates large WPT updates, so use the complete
diff when necessary:

```text
https://github.com/web-platform-tests/wpt/compare/<old-sha>...<new-sha>.diff
```

Filter upstream changes to the WPT directories mapped by
`src/wpt/BUILD.bazel`. For an individual failure, inspect the changed file,
its commits, the associated pull request, and any linked specification change.

### Running WPT

The commands below show the correct repository labels. Add all flags required
by the active test-hygiene instructions.

For standalone workerd, run from the workerd root:

```bash
bazel test //src/wpt/... --test_size_filters=
```

For a submodule checkout, run from the parent repository root:

```bash
bazel test @workerd//src/wpt/... --test_size_filters=
```

Clearing `test_size_filters` ensures that enormous WPT targets are included.
The complete pattern runs default and all-autogates variants plus ESLint and
TypeScript checks.

Use an affected suite target while iterating. Include the required `@` suffix.
For standalone workerd:

```bash
bazel test //src/wpt:<suite>@ \
  --test_size_filters= \
  --test_output=errors
```

For a submodule checkout:

```bash
bazel test @workerd//src/wpt:<suite>@ \
  --test_size_filters= \
  --test_output=errors
```

Do not use test-case filter arguments. Resolve the log root with
`bazel info bazel-testlogs` or use the path printed by Bazel. Parent-repository
logs are nested beneath the external workerd repository.

Search logs first for these messages instead of reading them in full:

```text
Missing test configuration
unexpectedly failed
unexpectedly succeeded
Please update the test config
Test file ... not found
```

### Handling test suite-level changes

A suite is a discovered WPT JavaScript file. Its config key is the exact,
case-sensitive path relative to the `wpt_directory` declared in
`src/wpt/BUILD.bazel`.

Discovery excludes non-JavaScript files and JavaScript files whose paths
contain `.tentative.` or `/tentative/`.

Config keys MUST remain in ascending order as enforced by ESLint's `sort-keys`
rule. Compare the complete raw key: punctuation affects ordering, so `-` sorts
before `.`. Do not place a new key based only on visual grouping.

1. Check for renames or moves before treating files as additions and
   deletions. Transfer existing configuration across genuine renames.
2. Remove config keys for deleted suites.
3. Add each new suite as a sorted blank entry so its behavior can be observed:

```typescript
'path/to/example.any.js': {},
```

Discovery includes JavaScript helper files unless their immediate parent is
`resources`. Helpers outside those directories may need explicit omitted
configuration even when they register no tests.

Re-run the complete WPT set after reconciling suite keys. Repeat until there
are no missing or stale configuration errors.

After every config edit, run the aggregate
`//src/wpt:wpt-all@tsproject@eslint` target with the repository prefix required
by the checkout. Do not rely only on suite-specific ESLint targets.

### Handling test changes

A subtest is an individual named `test()` or `promise_test()` within a suite.

1. Check whether removed and added names represent a renamed subtest.
2. When an expected failure now passes, determine whether workerd improved or
   upstream renamed, removed, or weakened the test before removing the
   expectation.
3. Investigate every new or newly failing subtest before classifying it.

Re-run affected suites while iterating, then rerun the complete WPT set until
there are no unexpected failures or successes.

### Analyzing failing tests

For each unexpected result:

1. Read the exact subtest failure and stack.
2. Inspect the upstream file at the new pinned SHA.
3. Identify the commit and specification change that introduced it.
4. Inspect the relevant workerd implementation or dependency.
5. Decide whether the test is relevant, unsafe to run, or inapplicable.
6. Record the current cause and any out-of-scope fix direction.
7. Compare default and all-autogates behavior before using one shared entry.

Do not classify an uncertain result merely to make the target pass. Runtime
fixes are out of scope; explain the likely fix in the report instead.

### META and load failures

META scripts and top-level code execute before subtests are registered. An
`expectedFailures` entry cannot classify a missing resource or evaluation
error at this stage. Partial `disabledTests` or `omittedTests` arrays cannot
avoid it either.

Whole-file `disabledTests: true` and `omittedTests: true` skip evaluation
before META imports. Use either only when its classification is independently
correct. Do not omit a relevant suite merely because harness support is
missing.

### Classifications

* `expectedFailures`: relevant subtests execute but currently fail assertions
  because workerd is non-conformant.
* `disabledTests`: relevant tests cannot run reliably or safely, such as hangs,
  crashes, state corruption, or harness limitations.
* `omittedTests`: tests are not applicable to workerd and are excluded from
  coverage.

Each property accepts `true` for a whole suite or an array of exact strings and
regular expressions. Prefer exact subtest names. Use a regex only when it is
narrow, anchored where practical, and checked against every intended name. Do
not use broad regexes or whole-suite `true` merely to make a target pass.

Every classification requires a `comment` describing the current technical
reason. Source comments must describe current behavior, not the investigation
or history that produced the entry. Commit messages and reports may include
the additional evidence and history.

The harness reports unmatched `expectedFailures`, but a regex can still hide
partially stale cases. It does not report stale disabled or omitted array
entries. Audit those manually against renamed and removed upstream subtests.

### Verification

Before completion:

1. Remove temporary `only` settings.
2. Confirm there are no missing or stale suite keys.
3. Confirm there are no unexpected failures or successes.
4. Manually audit changed disabled and omitted selectors.
5. Run the complete WPT target set using the applicable checkout command.
6. Require Bazel to report `Build completed successfully`.
7. Confirm the final `Executed N out of N tests: N tests pass` summary covers
   every selected target.
8. Do not treat an interrupted or partially analyzed invocation as success.
9. Run formatting and whitespace checks from the workerd root:

```bash
python3 tools/cross/format.py --check git
git diff --check
```

### Continuous improvement

Re-read the SKILL.md file. If there are any details that weren't clear or that
you had to look up separately, edit the file to include them. Information that
concerns how the WPT test framework works within workerd is valuable, but
refrain from adding info about specific situations you encountered during a
WPT update, as this is transient and not likely to be relevant to future WPT
updates.

### Final report

Report:

* The old and new WPT tags and pinned upstream comparison URL.
* Added, removed, and renamed suites.
* Expectations added or removed.
* Each classification and its durable technical reason.
* Relevant upstream and workerd source locations.
* Proposed out-of-scope fixes and unresolved items.
* Exact validation commands and final executed counts.
* Final Git state.

### Git handoff

Keep the generated dependency bump and WPT triage in separate commits.
