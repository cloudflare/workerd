---
description: Run tests or samples
subtask: true
---

Run: $ARGUMENTS

**Parse the argument to determine what to run:**

### Running tests

- `/run test <name>` — Run a specific test by name or path
- `/run tests` — Run all tests (`just test` or `bazel test //...`)
- `/run test <area>` — Run tests for an area like `streams`, `http`, `jsg`, `node`, etc.

**Steps for running a specific test:**

1. **Resolve the test target.** If the argument is a file path, find its Bazel test target. If it's a short name (e.g., `streams`), search for matching test targets:

   ```
   bazel query 'kind(".*_test", //src/workerd/...)' --output label 2>/dev/null | grep -i '<name>'
   ```

2. **Determine the test type:**
   - If the target comes from `wd_test()`, it's a `.wd-test` config test. These have variants: `@` (oldest compat), `@all-compat-flags` (newest compat), `@all-autogates`.
   - If the target comes from `kj_test()`, it's a C++ unit test.

3. **Run it.** Use `just stream-test` for a single test (streams output live) or `just test` for batches:

   ```
   just stream-test //src/workerd/api/tests:<target>@
   ```

   For running all tests in an area:

   ```
   just test //src/workerd/api/tests/...
   just test //src/workerd/jsg/...
   ```

4. **If the test fails**, read the output and provide a brief summary of what failed and why. If it's a compilation error, identify the source. If it's a test assertion failure, identify the failing test case and expected vs actual values.

### Running samples

- `/run sample <name>` — Run a sample by name (e.g., `helloworld_esm`, `nodejs-compat`, `durable-objects-chat`)
- `/run samples` — List all available samples

**Steps for running a sample:**

1. **List available samples** if the user says `/run samples` or the name doesn't match:

   ```
   ls samples/
   ```

2. **Find the sample config.** Look for `config.capnp` in `samples/<name>/`:

   ```
   ls samples/<name>/config.capnp
   ```

3. **Build workerd first:**

   ```
   bazel build //src/workerd/server:workerd
   ```

4. **Run the sample:**

   ```
   bazel run //src/workerd/server:workerd -- serve $(pwd)/samples/<name>/config.capnp
   ```

   Add optional flags if the user requests them or the sample requires them:
   - `--verbose` — verbose logging
   - `--watch` — auto-reload on file changes
   - `--experimental` — enable experimental features

   Some samples require `--experimental` (check if the config uses experimental compat flags). Samples that typically need it: `pyodide*`, `repl-server*`, `filesystem`.

5. **Tell the user** what address the sample is listening on (usually `http://localhost:8080` based on the config's socket definition) and that it will keep running until they press Ctrl+C.

6. **IMPORTANT:** Running a sample launches a long-lived process. After starting it, inform the user and do not attempt further actions until they indicate the process has been stopped. The intent is for the human to manually test the running server.

7. When the user indicates they are done testing the sample (e.g., "I'm done testing, you can stop now"), stop the process.

### Running all tests

- `/run tests` → `just test` (runs the full test suite)

This will take a long time. Warn the user and ask for confirmation before running.

### Running involves building

Running tests or samples will trigger a build of the necessary targets. If the build fails, report the failure and do not attempt to run.

### Run modes

- If the user requests to run with ASAN, add the `--config=asan` flag to the bazel command. Warn the user that ASAN builds are slower and may produce more verbose output.
- If the user requests to run with release mode, add the `--config=opt` flag to the bazel command. This will produce optimized binaries without debug symbols, which may be faster but harder to debug if something goes wrong.
- If the user requests to run with debug mode, add the `--config=debug` flag to the bazel command. This will produce debug builds with symbols, which are easier to debug but may be slower.

### Examples

| Command                                    | What it does                                          |
| ------------------------------------------ | ----------------------------------------------------- |
| `/run test streams`                        | Runs all streams-related tests                        |
| `/run test http-test`                      | Finds and runs the HTTP test target                   |
| `/run test //src/workerd/jsg:jsg-test`     | Runs the exact Bazel target                           |
| `/run tests`                               | Runs the entire test suite                            |
| `/run sample helloworld_esm`               | Builds workerd and runs the ESM hello world sample    |
| `/run sample nodejs-compat --experimental` | Runs the Node.js compat sample with experimental flag |
| `/run samples`                             | Lists all available samples                           |
