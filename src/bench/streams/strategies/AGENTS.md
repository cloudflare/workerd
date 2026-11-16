# Queuing strategy benchmarks

`CountQueuingStrategy` and `ByteLengthQueuingStrategy`, under both
implementations. Suite rules: `src/bench/streams/AGENTS.md`.

## Module map

| Module | Group | Cases |
| --- | --- | --- |
| `strategy-objects.js` | `strategyObjects` | 16 operations per call, synchronous: constructing each strategy, its `size()`, and its `highWaterMark` getter |
| `queue-accounting.js` | `queueAccounting` | A `ReadableStream` with 256 chunks enqueued in `start()` and read to the end, and a `WritableStream` with 256 queued writes, each with no strategy, each strategy, and a `size()` function |

## Comparing

- The strategy-object cases are the suites' only synchronous ones: they
  queue no microtasks. Every result goes through `b.blackBox()`, whose cost
  per call the report shows in the group's header, the same in both cells.
- Under C++, every access to a strategy's `size` mints a new function,
  where the TypeScript implementation returns one shared function
  (strategies test ledger #1). The `size()` cases, which read `size` for
  each call, therefore also measure creating a function under C++. A stream
  reads its strategy's `size` once, when it is constructed, so
  `queueAccounting` doesn't pay this per chunk.
