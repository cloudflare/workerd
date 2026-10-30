# Identity stream benchmarks

`IdentityTransformStream` and `FixedLengthStream`, under both
implementations. Suite rules: `src/bench/streams/AGENTS.md`.

The byte shapes are `256 x 64 B`, `64 x 4 KiB` and `16 x 64 KiB`
(`SHAPES` in `helpers.js`). Writes copy their bytes, so each size's chunk
is one shared buffer.

## Module map

| Module | Group | Cases |
| --- | --- | --- |
| `construction.js` | `construction` | 16 constructions per call: `IdentityTransformStream`; `FixedLengthStream`; `IdentityTransformStream`, then `getWriter()` and `getReader()` |
| `roundtrip.js` | `roundtrip` | 16 round trips per call on long-lived streams: `write()` + `read()` of 64 B, 4 KiB and 64 KiB, with a default reader and with a BYOB reader |
| `throughput.js` | `throughput` | Construct, queue every write, close, while the readable side is read to the end: an `IdentityTransformStream` and a `FixedLengthStream` with default readers, a BYOB reader, and `readAtLeast(4 KiB)` (the first two shapes) |
| `pipe.js` | `pipe` | `pipeTo()` from a value stream into an `IdentityTransformStream`, and `pipeThrough()` one from a byte stream, read to the end |

## Comparing

- The implementations split and merge writes into reads differently, so
  the cases count bytes, not reads. Over several queued writes, a BYOB
  read takes one write's bytes under C++ (stopping once a minimum is met),
  while the TypeScript implementation fills the view with everything
  already written (identity test ledger #22). The `BYOB reader` and
  `readAtLeast` cases therefore do fewer reads under TypeScript for the
  small shapes.
- A write settles once its bytes have been read, in both implementations,
  so every case reads concurrently with its writes.
