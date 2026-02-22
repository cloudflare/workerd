---
description: Find the Bazel test target for a source file
subtask: true
---

Find the Bazel test target(s) for the file: $ARGUMENTS

Steps:

1. Determine the directory containing the file.
2. Look for a `BUILD.bazel` file in that directory (or the nearest parent with one).
3. Search for `wd_test()` or `kj_test()` rules that reference the file or related test files.
4. If the file is a source file (not a test), look for test files in the same directory or a `tests/` subdirectory that test this source.
5. Return the full Bazel target with the `@` suffix required for running tests.

Output format:

- The exact `just test` or `bazel test` command to run
- List all variants if relevant (`@`, `@all-compat-flags`, `@all-autogates`)
- If no test target is found, suggest how to create one using `just new-test`
