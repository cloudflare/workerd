# Rust versus C++ JSG benchmarks

This package compares Rust and C++ JSG bindings for method calls, properties, conversions,
callbacks, errors, object creation and destruction, and garbage collection.

## Run

```sh
bazel build --config=benchmark //src/rust/jsg-bench:bench-jsg

# Run every case once, checking results and Fast API calls.
JSG_BENCH_VERIFY=1 bazel-bin/src/rust/jsg-bench/bench-jsg --benchmark_dry_run=true

# Measure. Any Google Benchmark flag works; --benchmark_filter selects cases.
bazel-bin/src/rust/jsg-bench/bench-jsg --benchmark_filter='/dispatch/'
```

These commands use the [optimized benchmark configuration](../../../docs/benchmarking.md); see there
for machine setup. On Linux, add `--config=cross-lto`, so that Rust takes part in ThinLTO and calls
between Rust and C++ can be inlined; without it, Rust/C++ ratios are larger. The `bazel-bin` symlink
points to the most recently used build configuration. The executable is marked `manual` and has no
short-run arguments, so `bazel run` measures normally; this also excludes it from CodSpeed.

With `JSG_BENCH_VERIFY` set, the executable counts Fast API calls and aborts at the first wrong
result or missed fast callback. Counting adds work to every call, so don't set it when measuring.
Rust JSG counts fast calls only in builds with debug assertions: the `RustFast/` cases are checked
with `--config=rust-debug`, and other builds log a warning and check only the C++ cases. With
`JSG_BENCH_NATURAL_JIT` set, V8 decides when to optimize the JavaScript instead of being told to.

The `smoke-test` target also sets `JSG_BENCH_SMOKE=1` to use small inputs and one operation per
batch, without timed warmup. It retains result, optimization, and Fast API checks. Performance
runs use the full input sizes and warmup. The target is tagged `workerd-benchmark`, so CI
configurations that skip benchmarks skip it too.

## Compare Rust with C++

`analyze.py` turns Google Benchmark JSON reports into per-call times and Rust/C++ ratios. Each
report counts as one process, so run the executable several times and keep every report:

```sh
out=.local/rust-jsg-bench/copy-in
mkdir -p "$out"
for i in 1 2 3 4 5; do
  bazel-bin/src/rust/jsg-bench/bench-jsg \
    --benchmark_filter='/buffer/copy-in/' \
    --benchmark_repetitions=5 \
    --benchmark_enable_random_interleaving=true \
    --benchmark_out="$out/process-$i.json" \
    --benchmark_out_format=json
done
python3 src/rust/jsg-bench/analyze.py "$out"
```

The analyzer needs Python 3.9 or later and no other packages. It reads every `process-*.json` in
the directory and prints a Markdown table. It also writes `summary.json`, with each process's
medians and ratios, and `summary.csv`, with one row per comparison. It compares each `Cpp/`,
`CppFast/` and `CppStructFast/` case with the matching `Rust/`, `RustFast/` or `RustStructFast/`
case.
It exits with an error if a case failed, or if a C++ case has no Rust counterpart from the same
processes, so a filter must select both versions of a case. Other cases, such as `Metadata/`,
are ignored.

## Run on a remote executor

`bazel test` runs benchmarks only once, without measuring, and `bazel run` on macOS with
`--config=bazelflare` builds a Linux executable that can't run locally. The manual `bench-report`
target runs the benchmark on the test executor instead and keeps the reports as test outputs:

```sh
bazel test --config=bazelflare --config=benchmark --config=cross-lto \
  --test_size_filters= --test_tag_filters= --build_tag_filters= \
  --remote_download_outputs=toplevel --test_timeout=3600 \
  --test_env=BENCH_FILTER='/dispatch/' --test_env=BENCH_PROCESSES=3 \
  //src/rust/jsg-bench:bench-report
out=.local/rust-jsg-bench/remote
rm -rf "$out" && mkdir -p "$out"
cp bazel-testlogs/src/rust/jsg-bench/bench-report/test.outputs/process-*.json "$out"
python3 src/rust/jsg-bench/analyze.py "$out"
```

`BENCH_FILTER` is passed to `--benchmark_filter` and defaults to every case; `BENCH_PROCESSES`
defaults to 3. One test must finish within `--test_timeout`, and the full suite takes about 13
minutes per process on a 4-vCPU executor, so select the cases you need. Bazel caches a passed test,
so add `--nocache_test_results` to repeat a run.

Executors differ in speed, so compare only reports from the same run. Two executables that differ
only in code layout can differ by 10-15% in a Rust case, so repeat a run before treating a change
of that size as real.

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
- `Cpp/` and `Rust/` use ordinary binding calls. `CppFast/` and `RustFast/` enable V8's Fast API
  for methods with primitive arguments and results, and the analyzer compares them with each
  other. `CppStructFast/` and `RustStructFast/` enable the `fast_jsg_struct` flag, with which both
  create returned structs from cached templates, and the analyzer compares them with each other.
  Verification runs count calls to confirm that the Fast API cases take the fast path;
  performance runs do not count calls.
- Rust compatibility flags are disabled, except `fast_jsg_struct` in the `RustStructFast/` cases.
  The fixtures do not use the Rust test harness because it enables V8's predictable mode.
- The JavaScript benchmarks run a batch of calls per timed iteration. The analyzer divides the
  time reported by Google Benchmark by `ops/batch` to get time per call. CPU time and elapsed
  time are both reported; time spent in the JavaScript loop is included. GC cases and
  `resource/new-wrap` and `resource/constructor` measure CPU time across the process, including
  V8's background threads; Google Benchmark appends `/process_time` to their names, which the
  analyzer removes. Other cases measure CPU time on the benchmark thread.
- For each process, the analyzer divides the median Rust time by the median C++ time. It reports
  the median of those ratios and their smallest and largest values. This is the observed range,
  not a confidence interval.
- `baseline/js-loop` runs the same JavaScript loop without binding calls in both versions, as a
  check on measurement noise. `gc/empty` measures collection without extra live objects, as a
  comparison for the other GC cases.
- Both buffer-copy implementations allocate new storage and copy the bytes. The length and sum
  cases read existing storage. String sizes are in UTF-16 code units; native strings are UTF-8.
  Ordinary numeric arrays use Rust `Vec<f64>` and C++ `kj::Array<double>`.
- Integer results, such as lengths, counts and sums of bytes, use Rust `u32` and C++ `uint32_t`;
  results that are numbers in general (`number`, `union`, the array and struct cases, and the
  callback) use `f64` and `double`. A fast integer argument reaches Rust as a `double` and
  converts with the slow path's checks, while C++ JSG receives it as a `uint32_t`, so the
  `dispatch/u32` cases compare different argument conversions as well. The Rust callback checks and casts `Local<Value>` to `Function`.
  The C++ callback benchmark calls V8 directly, not through `jsg::Function`.
- The `promise/` cases call methods that return a Rust `jsg::Promise` or a C++ `jsg::Promise`,
  and each call also runs the microtasks, so the reactions it adds run inside the timed loop.
  `untyped-void` returns a promise without a typed value to convert, as a floor for the others.
  Both isolates capture throws as rejections, as the `capture_async_api_throws` compatibility
  flag makes them do; `throw-captured` measures a method whose callback throws.
- `allocate-collect/N` times the creation of N objects followed by garbage collection. Other
  collection benchmarks keep objects alive. `native/lifetime/N` times the creation of one
  object, N reference clones, and cleanup.
- `Metadata/native-memory-*` records live allocations, not performance. These cases use macOS
  allocator statistics and are absent on Linux. They include the containers holding object
  references but exclude JS wrappers and do not measure total process memory.

These benchmarks measure individual binding operations, not Worker request latency.
