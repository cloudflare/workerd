# Writable stream benchmarks

`WritableStream` and its default writer, under both implementations. Suite
rules: `src/bench/streams/AGENTS.md`.

## Module map

| Module | Group | Cases |
| --- | --- | --- |
| `construction.js` | `construction` | 16 constructions per call: no sink; start, write, close and abort; a `CountQueuingStrategy`; then `getWriter()` |
| `writer.js` | `writer` | 16 operations per call on long-lived streams: `getWriter()` + `releaseLock()`; `write()` to a sync and to an async sink; `await ready`, then `write()` |
| `write-loop.js` | `writeLoop` | Construct, write every chunk, close: each write awaited (sync sink x16, x256; async sink x256); every write queued before awaiting the close (the same sinks); `await ready` before each write to an async sink, high-water mark 1 and 16 (x256) |
| `abort.js` | `abort` | `abort()` an idle stream, and one with 16 writes queued behind a sink that hasn't started, which the abort rejects |
