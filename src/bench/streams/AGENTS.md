# src/bench/streams/

Comparative benchmarks of the two streams implementations, the legacy C++
one (`src/workerd/api/streams/`) and the TypeScript one
(`src/per_isolate/webstreams/`), organized like `src/tests/streams/`: one
subdirectory per functional area. Every benchmark runs the same code under
both implementations, so the two reports compare case by case. Correctness
and parity are `src/tests/streams/`'s job; these suites only measure.

## Structure of a suite directory

```
readable/
  readable-modules.capnp  # THE module list, defined once (see below)
  readable-cpp.wd-bench   # runs the modules against the C++ implementation
  readable-ts.wd-bench    # runs the *same* modules against the TS implementation
  main.js                 # entry point: explicit re-exports of every group
  helpers.js              # the suite's shared helpers
  <shape>.js              # one group of related cases per file
  BUILD.bazel             # wd_bench() targets over glob(["*.js"]) + the .capnp
  AGENTS.md               # the suite's groups, and what to know comparing them
```

- Each named export of `main.js` is an entrypoint with a `bench()` handler,
  which registers a group of cases with `b.run()`. The report names a group
  `<service>:<export>` (`streams-readable-ts:readLoop`). Case names are the
  same in both cells, so the two reports line up by name.
- **`main.js` uses explicit named re-exports, never `export *`.** A name
  collision between two modules must be a load-time SyntaxError, not a
  silently dropped group.
- **The module list is defined exactly once**, as a
  `List(Workerd.Worker.Module)` constant in `<name>-modules.capnp` that both
  configs reference, so the cells cannot embed different code. A new module
  is added in two places only: the `.capnp` constant and `main.js`.

## Configs

The configs set no `compatibilityDate` and pin no date-gated flags.
`wd_bench()` owns the date axis: the default variant `name@` runs at the
newest date the build supports (`--compat-date=latest`), where every
date-gated streams flag the test suites pin is already on, so the C++ cell
measures what a newly created worker gets. `@all-compat-flags` runs at
2999-12-31. (The test suites pin those flags because their default variant
runs at 2000-01-01.)

The TS cell adds only what selects the implementation: the
`typescript_implemented_streams` flag, `--experimental` in its
`wd_bench()` (the flag is experimental), and the
`workerd-autogate-per-isolate-javascript-bootstrap` autogate, without which
workerd refuses to start a worker that sets the flag. The flag alone
selects the implementation: the C++ cell's `@all-autogates` variant enables
the autogate too and still runs C++ streams. Unlike the test suites' TS
cells, the TS cells don't set the catch-all `experimental` flag, which would
give them in-development APIs that the C++ cells lack.

## Writing cases

- **Same code, same work.** Cases never branch on the implementation.
  Where the implementations legitimately differ, such as in how they split
  bytes into reads or when a promise settles, a case relies only on
  behavior both share: it counts bytes rather than reads, and it never
  waits alone on a promise whose timing diverges. A divergence that changes
  the work an implementation does for the same code (C++ allocating a BYOB
  request for every default read of a byte stream, for example) is noted in
  the module's header comment and the suite's `AGENTS.md`, since it
  explains part of the difference between the reports.
- **Check the work.** Every case checks what it consumed (a chunk count, a
  byte total, a digest's size) and throws otherwise, so a stream that ends
  early fails the case, and the smoke test, instead of reporting a fast
  time.
- **Stream operations are timed asynchronously.** A case that touches a
  stream returns a promise, so the microtasks the operation queues run
  inside the timed call. A synchronous case would stop the clock before
  them, favoring whichever implementation defers more work. Only
  `strategies/`'s strategy-object cases, which queue nothing, are
  synchronous, and their results go through `b.blackBox()`. Stream
  operations have side effects that V8 can't eliminate, so they need no
  `b.blackBox()`.
- **Stay off the measurement floor.** The runner subtracts its own cost
  per call, which for an async case is about that of one stream read. A
  case whose operation costs about that much repeats it (16 constructions,
  16 reads) and says so in its name (`x16`). Otherwise the report flags it
  as at the measurement floor, and the result is mostly the runner's.
- **Steady state or whole lives.** Cases on long-lived streams (the
  `reader`, `writer` and `roundtrip` groups) build their streams in
  `bench()` and measure operations on them, leaving each stream as they
  found it. The other cases build, consume and close streams in every call;
  their counts (`x16`, `x256`) or shapes (`256 x 64 B`, `64 x 4 KiB`,
  `16 x 64 KiB`) separate fixed costs from per-chunk costs.
- Setup that cases need, such as payloads and expected results, happens
  untimed in `bench()`, which may be async; the cases run once it returns.

## Running

```
just wd-bench //src/bench/streams/readable:readable-cpp@
just wd-bench //src/bench/streams/readable:readable-ts@
just wd-bench //src/bench/streams/readable:readable-ts@ '*:readLoop/pull*'
just wd-bench //src/bench/streams/body:body-ts@ --format=json --output=/tmp/ts.json
```

`just wd-bench` builds with `--config=benchmark`. Compare the two cells'
reports from the same build on the same machine. A cell's groups run in
`main.js` order in one isolate, so a filtered run gives the JIT different
feedback from a full one: compare runs made with the same filter.

`bazel test //src/bench/...` runs each cell's `@smoke` variants, which run
every case briefly (`--quick`) to check that it works, and the lint. CI runs
them; it doesn't run the benchmarks themselves.

## Suites

| Directory | Subject |
| --- | --- |
| `readable/` | Value streams: construction, the read path, read loops, async iteration, `tee()`, `ReadableStream.from()`, cancellation |
| `readable-byte/` | Byte streams: default and BYOB readers, `respond()`, minimum reads, `autoAllocateChunkSize`, `tee()` |
| `writable/` | Construction, the write path, write loops with and without backpressure, `abort()` |
| `transform/` | Construction, round trips, throughput over a range of transformers |
| `piping/` | `pipeTo()`, `pipeThrough()` chains, pipe options, byte stream pipes |
| `identity/` | `IdentityTransformStream` and `FixedLengthStream`: round trips, throughput with default, BYOB and `readAtLeast()` reads, pipes into them |
| `encoding/` | `TextEncoderStream`, `TextDecoderStream`, and the two piped together |
| `compression/` | `CompressionStream` and `DecompressionStream` over chunk sizes and formats, and in body pipelines |
| `digest/` | `crypto.DigestStream` over chunk sizes and algorithms, written to and piped into |
| `strategies/` | The queuing strategy objects, and what a strategy costs a stream |
| `body/` | `Response` bodies in both directions, clones and tees, service binding round trips, `HTMLRewriter`, `formData()` |
