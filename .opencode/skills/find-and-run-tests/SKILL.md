---
name: find-and-run-tests
description: How to find, build, and run tests in workerd. Covers wd-test, kj_test target naming, bazel query patterns, and common flags. Also covers parent project integration tests if workerd is used as a submodule. Load this skill when you need to locate or run a test and aren't sure of the exact target name or invocation.
---

# Finding and Running Tests

## workerd Tests

### Test types

| Type              | File extension | BUILD macro | Target suffix                  |
| ----------------- | -------------- | ----------- | ------------------------------ |
| JS/TS integration | `.wd-test`     | `wd_test()` | None (target name = rule name) |
| C++ unit          | `*-test.c++`   | `kj_test()` | None                           |

### Finding targets

```bash
# Find test targets in a directory
bazel query 'kind("test", //src/workerd/api/tests:*)' --output label

# Find test targets matching a name
bazel query 'kind(".*_test", //src/workerd/...)' --output label 2>/dev/null | grep -i '<name>'

# Find tests that depend on a source file
bazel query 'rdeps(//src/..., //src/workerd/io:trace-stream, 1)' --output label 2>/dev/null | grep -i test
```

### Running

```bash
# Stream test output (preferred for debugging)
bazel test //src/workerd/api/tests:url-test --test_output=streamed

# Run with fresh results (no cache)
bazel test //target --test_output=streamed --nocache_test_results

# Run specific test case within a kj_test
bazel test //target --test_arg='-f' --test_arg='test case name'
```

### Common flags

| Flag                     | Purpose                                     |
| ------------------------ | ------------------------------------------- |
| `--test_output=streamed` | Stream test output to terminal in real time |
| `--nocache_test_results` | Force re-run, don't use cached results      |
| `--test_timeout=120`     | Override default test timeout (seconds)     |

---

## Parent Project Integration Tests

If workerd is used as a submodule in a parent project, that project may have its own integration test framework with different conventions. Load the `parent-project-skills` skill to discover those conventions.

### General principles that apply to any integration test framework

**Target naming with variant suffixes.** Some test macros generate multiple targets from a single source file by appending variant suffixes (e.g., `@`, `@all-autogates`, `@force-sharding`). If bazel says "is a source file, nothing will be built" or "No test targets were found", you likely need a suffix. Use `bazel query 'kind("test", //path:*)'` to discover the actual runnable target names.

**Cached results hide changes.** Always use `--nocache_test_results` when re-running after modifying test files or source code. Without it, bazel returns stale cached results with stale logs.

**Verify the feature actually ran.** After a test passes, search the test output for feature-specific evidence (script names, process types, subrequests, RPC calls). A passing test with no evidence the feature ran is not a valid test — see the `test-driven-investigation` skill.

### Debugging test failures

1. **Always use `--nocache_test_results`** when re-running after changes.
2. **Check test logs** at the path shown in bazel output: `bazel-out/.../testlogs/.../test.log`
3. **Search logs for feature-specific keywords** to verify the feature actually ran.
4. **Subrequest mismatches** (in frameworks that verify subrequests) typically show the actual vs expected request details — compare control headers carefully.
