# DigestStream benchmarks

`crypto.DigestStream`, under both implementations. Suite rules:
`src/bench/streams/AGENTS.md`.

The byte shapes are `256 x 64 B`, `64 x 4 KiB` and `16 x 64 KiB`
(`SHAPES` in `helpers.js`). Each case checks the digest's size and the
stream's `bytesWritten`.

## Module map

| Module | Group | Cases |
| --- | --- | --- |
| `construction.js` | `construction` | 16 per call: SHA-256 constructed; SHA-256 closed unwritten and its digest awaited |
| `write.js` | `write` | Every write queued, then close and the digest: SHA-256 in every shape; SHA-1, SHA-512, MD5 and crc32 in `64 x 4 KiB`; SHA-256 over string chunks |
| `pipe.js` | `pipe` | `pipeTo()` from a value stream in every shape, and from a `Response` body of 4 KiB, 64 KiB and 1 MiB |

## Comparing

- Both implementations drive the same native digest context, so the small
  chunks isolate the cost of the stream around it.
