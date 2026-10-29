# Readable byte stream benchmarks

`ReadableStream` with `type: 'bytes'`, under both implementations. Suite
rules: `src/bench/streams/AGENTS.md`.

The byte shapes are `256 x 64 B`, `64 x 4 KiB` and `16 x 64 KiB`
(`SHAPES` in `helpers.js`): many small chunks, a common network read size,
and large chunks.

## Module map

| Module | Group | Cases |
| --- | --- | --- |
| `construction.js` | `construction` | 16 constructions per call: plain; with `autoAllocateChunkSize`; then `getReader({ mode: 'byob' })` |
| `default-reader.js` | `defaultReader` | Default reads of each shape: chunks enqueued in `start()`, or one per `pull()` |
| `byob-reader.js` | `byobReader` | BYOB reads of each shape, reusing the returned buffer: the source filling the views with `respond()`, and reads copying from enqueued chunks; `read(view, { min })` and `readAtLeast()` needing four `respond()`s per read; and 16 reads per call from long-lived streams, by view size |
| `auto-allocate.js` | `autoAllocate` | Default reads of each shape with `autoAllocateChunkSize`, the source filling the allocated views |
| `tee.js` | `tee` | Both branches of each shape read |

## Comparing

- For a default read of a byte stream without `autoAllocateChunkSize`, C++
  allocates a 4 KiB BYOB request (16 KiB under an autogate) where the
  TypeScript implementation, like the spec, has none (readable-byte test
  ledger #5). `defaultReader`'s `pull()` source enqueues regardless, which
  both accept, so under C++ each pull also allocates a request that the
  enqueue discards.
- The `respond()` sources are used only for reads that have a BYOB request
  under both implementations: BYOB reads, and default reads with
  `autoAllocateChunkSize`.
