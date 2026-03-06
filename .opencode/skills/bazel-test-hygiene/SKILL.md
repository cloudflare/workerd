---
name: bazel-test-hygiene
description: Mandatory rules for running bazel tests during development. Load this skill before running any bazel test command, especially when validating fixes or verifying regression tests. Prevents false confidence from cached results, filter flags that silently match nothing, and partial test runs that miss breakage.
---

# Bazel Test Hygiene

## The Three Rules

### 1. Always disable caching

```bash
bazel test //... --nocache_test_results
```

**Why:** Bazel's action cache can serve stale test binaries even after you edit source files. Without `--nocache_test_results`, you may be running the OLD binary and seeing OLD results. This is not hypothetical — it has caused real false-positive/false-negative confusion in this repo.

**Always include `--nocache_test_results`.** No exceptions.

### 2. Keep it simple — no filter flags

Do NOT use `--test_arg='-f'` or similar filter flags to run individual test cases.

**Why:** KJ test's `-f` flag silently passes when zero tests match. If you typo the filter or the test name changes, bazel reports "PASSED" with zero tests actually run. This gives completely false confidence.

**Run the full test target.** If you need to check a specific test, look for its name in the full output. If the full suite is too slow, run the specific test _target_ (e.g., `//src/workerd/api:streams/standard-test@`), not a filtered subset within a target.

### 3. Run the full suite before claiming done

A single test target passing does not mean you haven't broken something else. Fixes to shared code (queue.c++, standard.c++, common.h) can break tests in completely different directories.

**Before claiming any fix is complete:**

```bash
bazel test //... --nocache_test_results
```

Check the final summary line: `Executed N out of N tests: N tests pass.` All N must match. If any test fails, the fix is not done.

## Red-Green Verification for Regression Tests

When writing a regression test for a bug fix, you MUST verify the test actually catches the bug:

1. **Green:** Run `bazel test //... --nocache_test_results` — all tests pass (fix in place)
2. **Red:** Remove the fix, run `bazel test //... --nocache_test_results` — the new test(s) MUST fail
3. **Green:** Restore the fix, run `bazel test //... --nocache_test_results` — all tests pass again

If step 2 passes (test doesn't fail without the fix), the test is not testing what you think. Go back and fix the test.

**Do the red-green on the full suite**, not just the one target. This catches two problems at once: (a) the regression test actually detects the bug, and (b) the fix doesn't break anything else.

## Anti-Patterns

| Don't                                     | Do instead                                          |
| ----------------------------------------- | --------------------------------------------------- |
| `bazel test //target` (no cache flag)     | `bazel test //target --nocache_test_results`        |
| `--test_arg='-f' --test_arg='test name'`  | Run the full target, grep output for test name      |
| Run one target, claim fix is done         | Run `//...`, check all-pass summary                 |
| Claim "tests pass" from a previous run    | Run fresh, read fresh output                        |
| Trust filter-based "PASSED" at face value | Check that the expected test names appear in output |
