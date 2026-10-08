We use a combination of micro and macro benchmarks for performance testing workerd.

# Building benchmarks

Benchmarks should be built using `--config=benchmark` configuration, which builds a release
binary with additional debug info.

To obtain most consistent results it is recommended to disable CPU frequency scaling
(use "performance" governor https://wiki.debian.org/CpuFrequencyScaling)

# Micro benchmarks

Micro benchmarks are defined using `wd_cc_benchmark` bazel macro. Use `bazel run --config=benchmark`
on a defined target to obtain benchmarking results.

See example in [bench-json.c++](../src/workerd/tests/bench-json.c++)

# JavaScript benchmarks

`workerd bench` measures JavaScript in a Worker. A Worker exports a `bench` handler that
registers cases; the runtime times them, so benchmark code never needs a precise clock (which
Workers don't have).

```js
export default {
  bench(b, env, ctx) {
    const encoder = new TextEncoder();
    const input = 'a'.repeat(1024);
    b.run('encode 1k', () => b.blackBox(encoder.encode(input)));
    b.run('sha-256 1k', async () => {
      b.blackBox(await crypto.subtle.digest('SHA-256', encoder.encode(input)));
    }, { minTime: '2s' });
  },
};
```

`b.run(name, fn, options)` registers a case. Cases run in order after `bench()` returns (or its
promise resolves). If `fn` returns a promise, the runner awaits it before the next call. `fn`
must either always or never return a promise; a case that does both fails.

For each case, the runner:

1. Calls `fn` repeatedly for `warmup` (200ms by default), so V8 can optimize it.
2. Doubles the number of calls per batch until a batch takes at least `minTime / samples`
   (and at least 1ms). `batch` sets the number instead.
3. Times `samples` batches (50 by default), reading the monotonic and thread CPU clocks only
   between batches. Each sample is a batch's time divided by its number of calls.

Options, all optional:

| Option     | Default | Meaning                                                                      |
| ---------- | ------- | ---------------------------------------------------------------------------- |
| `minTime`  | `1s`    | Sampling budget. A number of milliseconds, or a string like `"500ms"`, `"2s"` |
| `warmup`   | `200ms` | Warmup budget                                                                |
| `samples`  | 50      | Number of batches to time                                                    |
| `batch`    | (none)  | Calls per batch, instead of calibrating                                      |
| `setup`    | (none)  | Called before each batch, untimed; its result is passed to every call of `fn` |
| `teardown` | (none)  | Called after each batch with the setup result (if any), untimed              |
| `skip`     | false   | Report the case as skipped                                                   |

`b.blackBox(value)` returns `value` through a native call that V8 can't see into. Pass results
through it so that V8 can't optimize away the work that computed them. `b.options` has the
command line's case `filter` and whether `--quick` was given.

## Running

Define benchmarks in a config like a `.wd-test` file (conventionally named `.wd-bench`) and add
them to a `BUILD.bazel` file:

```python
load("//:build/wd_bench.bzl", "wd_bench")

wd_bench(
    src = "text-encoder-bench.wd-bench",
    data = ["text-encoder-bench.js"],
)
```

For each compat variant (`name@`, `name@all-compat-flags`, `name@all-autogates`), this defines a
target for `bazel run` and an `@smoke` test (`name@smoke`, `name@all-compat-flags@smoke`, ...)
that runs each case briefly, so CI checks that benchmarks keep working. CI doesn't run the
benchmarks themselves. As for `wd_test()`, the config can declare a writable disk service named
`TEST_TMPDIR`, for example for Durable Object storage; every run gets a new empty directory for it.
To run one in an optimized build:

```
just wd-bench //src/workerd/api/tests:text-encoder-bench@
just wd-bench //src/workerd/api/tests:text-encoder-bench@ '*/encode ascii*' --format=json
```

Arguments after the target go to `workerd bench`, which can also be run directly:

```
workerd bench config.wd-bench [filter] [--format=text|json] [--output=path] [--quick]
    [--compat-date=date] [--all-autogates] [--trace]
```

`--compat-date` requires the workers in the config to omit `compatibilityDate`.

The filter has `workerd test`'s format, optionally followed by `/<case-glob>`: for example
`main:encoding/encode*`. `--quick` caps the budgets, to check that cases run rather than to
measure them.

## Reading the results

The text report has a table per handler with each case's median time per call, the 95%
confidence interval of the median, the mean, and the median thread CPU time (mostly noise on
Windows, whose thread CPU clock advances only on scheduler ticks). `--format=json`
writes the full report, including every sample, as defined by
[bench.capnp](../src/workerd/io/bench.capnp). The JSON format isn't stable yet.

- The runner's own cost per call (an empty function, and an empty async function for cases that
  return promises) is measured once per run and subtracted from the results. It is shown under
  each group's name.
- **at measurement floor**: the case took less than twice the runner's overhead, so the result is
  mostly the runner's cost. V8 may have optimized away the work; pass results through
  `b.blackBox()`.
- **high variance**: the coefficient of variation exceeds 10%. Try more `samples` or a longer
  `minTime`, and check that the machine is quiet.

Results compare runs on the same machine and build. They don't predict production latency. The
report records the build mode, CPU, and frequency governor, and warns about a debug or sanitizer
build or a governor other than "performance". Use `--config=benchmark` (as `just wd-bench` does).

Tail workers don't run under `workerd bench`, since they would add their cost to every
subrequest. `--trace` runs them, to measure that cost.

## Classes

Only plain-object exports, like the default export above, have `bench` handlers. The methods of
`WorkerEntrypoint` and `DurableObject` classes are RPC methods, so a method named `bench` on one
is an ordinary RPC method that `workerd bench` doesn't run. To measure a Durable Object or an RPC
entrypoint, call it through a binding from a plain object's `bench` handler.

See [text-encoder-bench.js](../src/workerd/api/tests/text-encoder-bench.js).

