# Readable value stream benchmarks

`ReadableStream` with the default controller, under both implementations.
Suite rules: `src/bench/streams/AGENTS.md`.

## Module map

| Module | Group | Cases |
| --- | --- | --- |
| `construction.js` | `construction` | 16 constructions per call: no source; start, pull and cancel; a `CountQueuingStrategy`; a `size()` strategy; then `getReader()` |
| `reader.js` | `reader` | 16 operations per call on long-lived streams: `getReader()` + `releaseLock()`; `read()` pulled on demand (high-water mark 0), from a queue `pull()` refills, with an async `pull()`; `enqueue()` then `read()`, and `read()` then `enqueue()` |
| `read-loop.js` | `readLoop` | Construct, read to the end, close: chunks enqueued in `start()` (x1, x16, x256); one per `pull()` (x16, x256, and x256 with high-water mark 16); one per async `pull()` (x16, x256) |
| `async-iteration.js` | `asyncIteration` | `for await` over the `readLoop` sources, and a `break` after the first of 16 chunks, which cancels the stream and runs its source's `cancel()` |
| `tee.js` | `tee` | Both branches read (enqueued x16, x256; `pull()` x256); one read while the other is cancelled; both cancelled |
| `from.js` | `from` | `ReadableStream.from()` over an array (x16, x256), a sync generator, and an async generator (x256) |
| `cancel.js` | `cancel` | `cancel()` idle, with a pending read, discarding 256 queued chunks; `error()` with a pending read |

## Comparing

- A tee branch's `cancel()` settles only once the source finishes or the
  other branch cancels too under the TypeScript implementation, as in the
  spec; C++ settles it at once. `read one branch, cancel the other` awaits
  the cancel together with the other branch's read, which both
  implementations complete.
- Once a source has called `close()`, C++ cancels the stream without
  running the source's `cancel()`, even while chunks are still queued; the
  TypeScript implementation runs it, as the spec requires, since the source
  keeps its algorithms until the queue drains. `break after the first of
  16`'s source therefore doesn't close, so that both run its `cancel()`,
  which the case checks.
