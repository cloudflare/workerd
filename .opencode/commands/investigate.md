---
description: Investigate a bug from a Sentry issue or error description, biasing toward writing a reproducing test early
subtask: false
---

Load the `test-driven-investigation`, `investigation-notes`, `find-and-run-tests`,
`parent-project-skills`, and `dad-jokes` skills, then investigate: $ARGUMENTS

## Prerequisites

Use the Sentry MCP tools when given a Sentry issue ID or URL. The Sentry MCP connection requires a
user-specific `X-Sentry-Token` header configured in `~/.config/opencode/opencode.json` under
`mcp.sentry.headers`. If the Sentry tools fail with auth errors, tell the user to check their token
configuration and stop — do not guess at issue details.

## Parsing the argument

The argument can be:

- **A Sentry issue ID** (e.g., `6181478`) — fetch from Sentry
- **A Sentry short ID** (e.g., `EDGEWORKER-RUNTIME-4MS`) — fetch from Sentry
- **A Sentry URL** (e.g., `https://sentry.io/organizations/.../issues/...`) — extract the issue ID,
  fetch from Sentry
- **A plain text description** (e.g., `"concurrent write()s not allowed" in kj/compat/http.c++`) —
  skip Sentry, go straight to orientation

## Steps

### 0. Create a tracking document

Create a tracking document in the `investigation-notes` tool to keep track of hypotheses, code read,
and test results. **Always** actively consult and update this document throughout to avoid losing
insights, going in circles, or forgetting what you've tried. See the "Investigation Notes" section
below for format and rules.

### 1. Extract the error

**If Sentry issue:**

1. Fetch the issue details with `sentry_get_sentry_issue`.
2. Fetch the most recent event with `sentry_list_sentry_issue_events` (limit 1), then
   `sentry_get_sentry_event` to get the full stack trace.
3. Extract:
   - The **error message** (assertion text, exception message, crash description)
   - The **assertion/crash site** (file and line from the top of the stack)
   - The **entry point** (the outermost workerd/KJ/capnp frame in the stack — where the operation
     started)
   - The **time range** of occurrences (when it started, if it's increasing in rate, etc.)
   - Identify the issue **status**: is it new, regressed, or longstanding

**If plain text:** Parse the error message and file reference from the description.

**Output to user:** The error message, crash site, and entry point, time range, and status.
One short paragraph. Do not go deeper yet.

### 2. Orient

Find three things:

1. **The crash site source.** Read the assertion/crash line and its immediate context (~50 lines).
   Understand what invariant was violated and what state would cause it. If the crash is in a C++
   class method, **use the `cross-reference` tool** to quickly locate the header, implementation
   files, JSG registration, and test files for that class.

2. **Recent changes.** If the incident being investigated started, re-occurred, or increased in rate
   recently, look at the git history around the crash site to see if recent changes may have caused
   the bug. Use `git blame` to find when the crash line or the code around it was last modified, and
   `git log` to see recent commits in that file.

3. **The test file.** Use `/find-test` on the source file containing the crash site (the
   cross-reference output may already list relevant test files). If no test exists, identify the
   nearest test file in the same directory.

4. **Existing feature tests.** Search for existing tests that exercise the _feature_ involved in the
   bug — not just tests near the crash site file. The crash may be in `pipeline.c++` but the relevant
   working test may be an integration test in a completely different directory. These existing tests
   encode setup, verification, and framework patterns you need. They are your starting template.

5. **The build command.** Construct the exact `bazel test` invocation to run a single test case from
   that test file.

**Output to user:** The crash site with a one-sentence explanation of the invariant, the test file
path, and the build command.

### 3. Hypothesize

Form a hypothesis in the format:

> "If I do X after Y, Z will happen because W."

This does not need to be correct. It needs to be testable. State it to the user.

Ask for clarification or additional details if you cannot form a hypothesis with the information
you have. But do not ask for more information just to delay writing a test.

### 4. Write the test

**Start from an existing test if one exists** (from step 2.3). Clone it and modify the single
variable that your hypothesis targets (disable an autogate, change a config flag, alter the setup).
This is almost always faster and more correct than writing from scratch, because existing tests
already have the right verification (subrequest checks, expected log patterns, shutdown handling).

If no existing test is suitable, write a new one that:

- Sets up the minimum state to reach the crash site
- Performs the operation described in the hypothesis
- **Includes observable verification** — the test must check that the feature actually ran, not
  just that nothing crashed. Use subrequest expectations, check for feature-specific log lines, or
  verify side effects.
- Asserts the expected behavior (what _should_ happen if the bug didn't exist)

Keep it short. Prefer public API. Do not try to reproduce the full production call stack.

Do not interrupt your flow to investigate tangents while writing the test. If you
realize you need to understand something else to write the test, make a note of it
and move on — you can investigate it in the next iteration if the test doesn't
reproduce the bug.

### 5. Run the test

Build and run using the command from step 2. **Start the build immediately.** Do not read more code
before starting the build.

Using parallel sub-agents, waiting for the build, read code that would inform the **next** test
iteration if this one doesn't reproduce the bug

### 6. Validate and iterate

After every test run:

1. **Always** update the tracking document (if using one)
2. **Always** check the test output for evidence the code path was exercised — feature-specific log
   lines, subrequests, RPC calls. A test that passes with no evidence the feature ran is not a valid
   result.

Based on the result:

- **Test fails as expected** → the mechanism is confirmed. Report findings to the user. Read code
  with purpose to find the fix, not to find the bug.
- **Test passes with evidence the feature ran** → hypothesis was wrong. Adjust the hypothesis,
  update the test, run again. Tell the user what you learned.
- **Test passes with NO evidence the feature ran** → the test is not exercising the code path. Do
  not read more source code to explain why. Fix the test first — compare it against existing working
  tests to find what's missing.
- **Test doesn't compile** → fix the compilation error and rerun. This is not a setback, it's a
  normal part of the process.
- **Test crashes differently** → follow the new trail but note the divergence. Tell the user.

Repeat until the bug mechanism is confirmed or you've exhausted reasonable hypotheses (at which
point, report what you've tried and what you've ruled out).

### 7. Report

When the mechanism is confirmed, output:

- **Bug summary**: One paragraph describing the root cause
- **Reproduction**: The test name and how to run it
- **Crash site**: `file:line` with explanation
- **Suggested fix direction**: Where the fix likely needs to go (if apparent from the test results)

## Rules

- **Always work in parallel whenever possible.** Don't wait for the build to finish before reading
  code that would inform the next test iteration. Use the build time to maximize your learning and
  progress. Investigate multiple hypotheses in parallel if you can, but do not let this delay
  writing and running tests.
- **Do not keep endlessly reading code before the first test is written and building.** If you hit
  15 tool calls, write whatever test you can with your current understanding.
- **Do not re-read the same function more than twice.** If you catch yourself doing this, update
  the tracking document to record findings and write a test immediately.
- **Do not trace the full call stack before writing a test.** The test will tell you if your
  understanding is correct.
- **Every hypothesis must be tested, not just reasoned about.**
- **Update the tracking document with each iteration.** If a tracking document is being used, update
  the hypotheses, code read, and test results sections so you have a clear record of your
  investigation process. After compaction, **always** update the tracking document before continuing
  to the next step.
- **Never** miss an opportunity for a good dad joke (using the `dad-jokes` skill). Don't overdo it.
  When summarizing, **always** preserve any jokes from the subagent output, and **always** including
  the intro prefix ("Here's a dad joke for you:", etc.) so the user knows it's intentional.
