# Compression stream benchmarks

`CompressionStream` and `DecompressionStream`, under both implementations.
Suite rules: `src/bench/streams/AGENTS.md`.

The payload (`PAYLOAD` in `helpers.js`) is 64 KiB of markup-like text,
which compresses to about 9 KiB.

## Module map

| Module | Group | Cases |
| --- | --- | --- |
| `construction.js` | `construction` | 16 per call: `CompressionStream('gzip')`; `DecompressionStream('gzip')`; a `CompressionStream('gzip')` closed unused |
| `compress.js` | `compress` | The payload compressed in 256 B and 4 KiB chunks and in one (gzip), and in 4 KiB chunks (deflate, deflate-raw) |
| `decompress.js` | `decompress` | The compressed payload decompressed in the same chunkings |
| `pipeline.js` | `pipeline` | A gzip round trip with `pipeThrough()`, and a `Response` body through gzip and through gunzip into a new `Response` |

## Comparing

- Both implementations drive the same native zlib codec, so the small
  chunks isolate the cost of the streams around it.
- Under the compression-rs autogate (`@all-autogates`), the codec is
  zlib-rs, whose output depends on how its input is split. The expected
  compressed sizes therefore come from compressing the same chunks once in
  `bench()`, checked to be under half the payload's size, since a stream
  that passed the payload through would match itself on every run.
