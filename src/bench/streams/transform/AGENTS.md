# Transform stream benchmarks

`TransformStream`, under both implementations. Suite rules:
`src/bench/streams/AGENTS.md`.

`TRANSFORMERS` in `helpers.js` names the transformers the cases compare:
none, `transform()` enqueuing each chunk, an async `transform()`, one
enqueuing each chunk twice, one dropping every other chunk, and `flush()`
enqueuing a last chunk. Each stream gets a new transformer.

## Module map

| Module | Group | Cases |
| --- | --- | --- |
| `construction.js` | `construction` | 16 constructions per call: no transformer; start, transform and flush; both strategies; then `getWriter()` and `getReader()` |
| `roundtrip.js` | `roundtrip` | 16 `write()` + `read()` round trips per call on long-lived streams, with no transformer, an enqueuing `transform()`, and an async `transform()` |
| `throughput.js` | `throughput` | Construct, write every chunk, close, while the output is read to the end: awaiting `ready` before each write (no transformer x16, x256; each other transformer x256), and queueing every write at once (no transformer, an enqueuing `transform()`; x256) |
