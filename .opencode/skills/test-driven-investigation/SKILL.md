---
name: test-driven-investigation
description: Use when investigating bugs, crashes, assertions, or unexpected behavior - requires writing a reproducing test early instead of over-analyzing source code; concrete experiments over mental models
---

# Test-Driven Investigation

## Overview

When investigating a bug, write a reproducing test as early as possible. Analysis without experimentation spirals into circular reasoning.

**Core principle:** A 20-line test that fails tells you more than 2 hours of reading source code.

## The Anti-Pattern

```
READ code → BUILD mental model → READ more code → REVISE model → READ more code →
SECOND-GUESS model → READ same code again → ... → eventually write a test
```

This feels productive but isn't. You're pattern-matching on code without grounding in reality. Each re-read adds uncertainty, not clarity.

## The Pattern

```
1. READ the error message / assertion / crash
2. IDENTIFY the minimal trigger (what operation, on what object, in what state?)
3. WRITE a test that sets up that state and performs that operation
4. RUN it
5. Let the RESULT guide the next step
6. Iterate as needed, but always grounded in test results backed by code, not just code.
```

Steps 1-2 should take minutes, not hours. You don't need to understand the full call chain to write a test. You need to know what the entry point is and what went wrong.

### Orientation

Before writing the test you need to know three things: what object/API to exercise, what test file to put it in, and how to build/run it. Spend up to 20-30 tool calls finding these. This is bounded research in service of the test -- not open-ended code analysis. If you don't know the exact API, pick the closest thing you can find and write the test anyway. A test that exercises the wrong API and passes still tells you something.

## Check the Commit History

Early in the investigation, check the recent commit history for changes that touched the relevant code. A bug that appeared recently was likely introduced recently. `git log --oneline -20 -- path/to/relevant/files` takes seconds and can immediately narrow your search from "something somewhere is wrong" to "this specific change might be the cause."

This is not a substitute for writing a test -- it's a way to form a better hypothesis faster. If you can identify a suspect commit, your first test should try to confirm whether that change is responsible.

**When you find a suspect commit:** Don't stop there. Always check at least 10 commits in either direction around it. Bugs are often introduced by the interaction between multiple changes, not a single commit. A commit that looks innocent in isolation may have broken an assumption that a nearby commit relied on. Looking at the surrounding commits also guards against confirmation bias -- you might find a better explanation in an adjacent change.

**Don't mistake correlation for causation.** A commit that lines up timewise with when the bug appeared is a lead, not a conclusion. It might be a coincidence -- the real cause could be an environmental change, a dependency update, a race condition that only became likely under new load patterns, or a latent bug exposed by an unrelated change. Treat a suspect commit as a hypothesis to test, not evidence of guilt. If you can't demonstrate the mechanism by which the commit causes the bug, you haven't found the cause.

**What to look for:**

- Changes to the file/function where the crash or assertion fires
- Changes to setup, config, or initialization code for the affected subsystem
- Changes to shared utilities or base classes used by the affected code
- Refactors that moved or renamed things in the area

**Keep it bounded:** This is around 5 tool calls of `git log` and `git show`, not an archaeology expedition. If nothing jumps out, move on and write the test with what you have. The commit history is one input to hypothesis formation, not a prerequisite for it.

## When You're Tempted to Read More Code

Ask yourself:

- **"Am I reading this to write a test, or to avoid writing one?"** If you can't articulate what the test would look like, that's the problem to solve -- not more reading.
- **"Do I have a hypothesis I can test?"** If yes, test it. If no, form the simplest one possible and test that.
- **"Do I have more than one hypothesis?"** Either pick one to test or work in parallel with several. Don't get stuck in "I need to understand everything before I can test anything" and don't try juggling multiple mental models in your head.
- **"Am I re-reading code I already read?"** Stop. You're stuck. Write a test with your current understanding, even if it's wrong. A wrong test that runs teaches you something. A correct mental model that you never test teaches you nothing.
- **"Am I retreading over the same path?"** If you find yourself tracing the same call stack multiple times, stop. Write a test that hits the point of failure directly. You can always adjust it later. If necessary, use a temporary tracking document to help you keep track of what you've already read so that you don't have to keep it all in your head or re-read the same code. But that document never takes priority over writing a test and running it.

## Start From Existing Tests

Before writing a test from scratch, find existing tests for the same feature or subsystem. Search for tests that exercise the API, protocol, or code path you're investigating — not just tests in the same file as the crash site.

Existing tests encode implicit knowledge you won't get from reading source code: required setup, framework-specific verification patterns, config quirks, shutdown handling. A test that's structurally wrong (missing verification, wrong config) will "pass" silently without exercising anything.

**Adapt an existing working test rather than inventing one.** If an existing test works for the feature and the bug is gated by a flag, autogate, or config change, the fastest reproduction is often to clone the working test and change the single variable.

## Verify the Test Exercises the Code Path

A test that passes is not evidence the feature worked. It may mean the feature never ran.

After every test run, check the output for evidence the specific code path was exercised — log lines mentioning the feature, subrequests being made, expected error messages, metrics being recorded. If you can't find evidence in the test output, the test is not valid regardless of its exit status.

**Concrete checks:**

- Does the test output mention the feature's script/worker/pipeline name?
- Are expected subrequests or RPC calls visible in the logs?
- If the feature produces side effects (trace delivery, storage writes, network calls), are those side effects observable?
- If you removed the feature config entirely, would the test still pass? If yes, the test isn't testing the feature.

## Scoping the Test

You don't need to reproduce the exact production scenario. You need to reproduce the _mechanism_.

**Production crash:** `KJ_REQUIRE(!writeInProgress, "concurrent write()s not allowed")`

**You DON'T need:** Every detail of the production call stack that potentially leads to this.

**You DO need:** To find the shortest path to trigger it, even if only hypothetically.

```
Good test scope:
  Create the pipe/adapter/object directly
  → Put it in the suspect state
  → Perform the operation that should fail
  → Assert what happens

Bad test scope:
  Reproduce the entire production call stack
  with all middleware and wrappers
```

## Forming a Hypothesis

A hypothesis for a bug investigation is:

> "If I do X after Y fails, Z will happen because the cleanup in Y's error path doesn't do W."

It does NOT need to be:

> "I have traced every code path and am certain that line 847 is the root cause because of the interaction between..."

The first version is testable in minutes. The second takes hours to construct and might still be wrong.

## After the Test

- **Test fails as expected:** You've confirmed the bug mechanism. Now you can read code _with purpose_ -- to find the fix, not to find the bug.
- **Test passes (bug doesn't reproduce):** Your hypothesis was wrong. That's valuable. Adjust and try again. This is faster than reading code for another hour.
- **Test crashes differently:** You found something else. Follow that trail, but do not abandon the original effort. You can have multiple parallel threads of investigation, but each should be grounded in test results, not just code reading.

## Red Flags -- You're Over-Analyzing

- You've read the same file/function more than twice
- You're building a multi-level mental model of "what calls what"
- You're writing detailed notes about code flow before writing any test code
- You say "let me understand X before I write the test" more than once
- You feel like you need to understand the entire system before you can test a single component
- A test "passes" but you have no evidence the feature ran — and you're reading code to explain why instead of fixing the test
- You've written multiple tests that all pass without reproducing the bug, and you haven't questioned whether any of them exercise the right code path

## Build Times Don't Change the Priority

In this codebase, a C++ compile-and-test cycle can take minutes. This does not justify delaying the test in favor of more code reading. It changes what "quick feedback" looks like:

- **Fast-compiling codebase:** Write test, run, see result in seconds, iterate rapidly.
- **Slow-compiling codebase:** Write test, start the build, use the wait time for targeted reading that serves the next iteration. The build is running -- you're not blocked, you're pipelining.

The temptation with slow builds is "I should be really sure before I compile." This is the analysis spiral in disguise. A test that doesn't reproduce the bug on the first try but compiles and runs is not wasted -- it's a known-good harness you can adjust in the next cycle, often with a much faster incremental rebuild.

## Applying to This Codebase

workerd and its dependencies (KJ, Cap'n Proto) have extensive test infrastructure:

- **KJ tests:** `KJ_TEST("description") { ... }` in `*-test.c++` files
- **workerd tests:** `.wd-test` format for JS/TS integration tests
- **Build/run:** `bazel test //path:target --test_arg='-f...' --test_output=all`

Most KJ/capnp bugs can be reproduced with a self-contained `KJ_TEST` using public API (pipes, streams, promises, HTTP). You rarely need internal access.

## The Bottom Line

**Write the test. Run the test. Analyze the results. Think. Iterate.**

Not the other way around.
