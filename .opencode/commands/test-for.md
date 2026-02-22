---
description: Find tests that exercise a source file's code, with specific test case details
subtask: true
---

Find tests for: $ARGUMENTS

Steps:

1. **Resolve the source file.** If the argument is a file path, use it directly. If it's a class or symbol name, find its source file first.

2. **Extract key symbols.** Read the source file (header preferred) and identify the main exported symbols: class names, public method names, free function names, macros. These are what we'll search for in tests.

3. **Find test targets.** Look in the `BUILD.bazel` file in the same directory and in a `tests/` subdirectory for `wd_test()` and `kj_test()` rules. Also check if the source file is a dependency of test targets:

   ```
   bazel query 'rdeps(//src/..., <source_target>, 1)' --output label 2>/dev/null | grep -i test
   ```

4. **Find test cases that reference the code.** For each test file found, grep for the key symbols from step 2. Read the surrounding context to understand what each test case exercises. Tests for some C++
code may be in JavaScript test files if the C++ code is exposed to JS, so check those as well.

5. **Assess coverage.** Compare the public API surface (from step 2) against what the tests exercise (from step 4). Identify:
   - Well-tested: symbols referenced in multiple test cases
   - Partially tested: symbols referenced but not thoroughly (e.g., only happy path)
   - Untested: public symbols with no test references

6. **Output:**
   - **Source**: file path and main symbols
   - **Test targets**: Bazel labels with `just test` commands
   - **Test cases** (grouped by test file):
     - `file:line` — description of what's tested (e.g., "tests TextEncoder with UTF-8 input")
   - **Coverage assessment**:
     - Well-tested: list of symbols
     - Gaps: list of public symbols with no apparent test coverage
   - **Suggestions**: specific test cases to add for untested code paths, if applicable
