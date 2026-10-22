# Rust versus C++ JSG benchmarks

This package compares Rust and C++ JSG bindings for method calls, properties, conversions,
callbacks, errors, object creation and destruction, and garbage collection.

## Run

```sh
bazel build --config=benchmark //src/rust/jsg-bench:bench-jsg

# Quick check that the benchmarks run and return the expected results.
bazel-bin/src/rust/jsg-bench/bench-jsg --benchmark_dry_run=true

# Check that the C++ Fast API benchmarks use the fast path.
JSG_BENCH_VERIFY_FAST=1 bazel-bin/src/rust/jsg-bench/bench-jsg \
  --benchmark_filter='^CppFast/' --benchmark_dry_run=true

# Measure buffer-copy performance in separate processes.
# Omit --filter to measure the full suite.
python3 src/rust/jsg-bench/run.py --output .local/rust-jsg-bench/copy-in \
  --filter='/buffer/copy-in/'
python3 src/rust/jsg-bench/analyze.py .local/rust-jsg-bench/copy-in

# Test the Python scripts.
python3 src/rust/jsg-bench/analyze-test.py
```

The commands above use the [optimized benchmark configuration](../../../docs/benchmarking.md).
The runner executes the built binary directly, separately from Bazel. The `bazel-bin` symlink
points to the most recently used build configuration; `--binary` selects another executable.
The `smoke-test` Bazel target runs every case with `--benchmark_dry_run` and verifies Fast API
calls. `tooling-test` runs the Python tests. The benchmark executable itself has no short-run
arguments and is marked `manual`, which also excludes it from CodSpeed.

The runner saves JSON results for each process, logs, command-line arguments, the executable's
hash, and machine information. Linux diagnostics include CPU details, kernel version, processor
count, and CPU frequency governors. It refuses to overwrite results and reports an error if no
benchmarks match the filter, measurements fail or are missing, or Fast API call counting is enabled.
`--processes`, `--repetitions`, and `--min-time` control the number and length of runs. With
`--natural-jit`, V8 decides when to optimize the JavaScript.

## How measurements work

- Each JavaScript benchmark creates a new V8 isolate and context and keeps both Rust and C++ objects alive.
  Both versions use the same inputs and JavaScript loop, but each loop calls only one version.
  Each case checks its result and the sum returned by the batch. The batch sum is checked again
  after timing ends, including for Fast API calls.
- JavaScript compilation, object setup, and at least 30 ms of warmup happen before timing starts.
  By default, the benchmark asks V8 to optimize the loop with TurboFan. Garbage collection stays
  enabled. Each batch has its own V8 handle scope, so temporary handles can be released between
  batches. Inputs are reused unless creating them is the operation being measured. Forced
  optimization runs check that the loop is still TurboFan-optimized before and after timing.
- `Cpp/` uses ordinary C++ binding calls. `CppFast/` enables V8's Fast API for primitive arguments.
  `CppStructFast/` enables cached templates for creating returned structs. The Fast API check
  above counts calls to confirm that they take the fast path; performance runs do not count calls.
- Rust compatibility flags are disabled. The fixtures do not use the Rust test harness because it
  enables V8's predictable mode.
- The JavaScript benchmarks run a batch of calls per timed iteration. The analyzer divides the
  time reported by Google Benchmark by `ops/batch` to get time per call. CPU time and elapsed
  time are both reported; time spent in the JavaScript loop is included. GC cases and
  `resource/new-wrap` and `resource/constructor` measure CPU time across the process, including
  V8's background threads. Other cases measure CPU time on the benchmark thread.
- For each process, the analyzer divides the median Rust time by the median C++ time. It reports
  the median of those ratios and their smallest and largest values. This is the observed range,
  not a confidence interval.
- `baseline/js-loop` runs the same JavaScript loop without binding calls in both versions, as a
  check on measurement noise. `gc/empty` measures collection without extra live objects, as a
  comparison for the other GC cases.
- Both buffer-copy implementations allocate new storage and copy the bytes. The length and sum
  cases read existing storage. String sizes are in UTF-16 code units; native strings are UTF-8.
  Ordinary numeric arrays use Rust `Vec<Number>` and C++ `kj::Array<double>`; `Vec<f64>` uses a
  typed array instead.
- Rust integer results use `Number`, while C++ returns `uint32_t`; arguments and stored values are
  integers in both. The Rust callback checks and casts `Local<Value>` to `Function`. The C++
  callback benchmark calls V8 directly, not through `jsg::Function`.
- `allocate-collect/N` times the creation of N objects followed by garbage collection. Other
  collection benchmarks keep objects alive. `native/lifetime/N` times the creation of one
  object, N reference clones, and cleanup.
- `Metadata/native-memory-*` records live allocations, not performance. These cases use macOS
  allocator statistics and are absent on Linux. They include the containers holding object
  references but exclude JS wrappers and do not measure total process memory.

These benchmarks measure individual binding operations, not Worker request latency.
