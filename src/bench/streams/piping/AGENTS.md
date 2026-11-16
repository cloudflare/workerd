# Piping benchmarks

`pipeTo()` and `pipeThrough()`, under both implementations. Suite rules:
`src/bench/streams/AGENTS.md`.

## Module map

| Module | Group | Cases |
| --- | --- | --- |
| `pipe-to.js` | `pipeTo` | A value stream into a writable stream: a sync sink (x16, x256); an async sink (x256), with a high-water mark of 16 too; a `pull()` source (x256) |
| `pipe-through.js` | `pipeThrough` | Chains of 1 and 4 `TransformStream`s, with no transformer (1: x16, x256; 4: x256) and with an enqueuing `transform()` (x256) |
| `pipe-options.js` | `pipeOptions` | A `signal` that never fires; `preventClose`, `preventAbort` and `preventCancel`; an abort after 64 chunks of an endless source |
| `byte-pipes.js` | `bytePipes` | A byte stream into a writable stream, in shapes `256 x 64 B`, `64 x 4 KiB` and `16 x 64 KiB` |

Pipes into identity streams are in `identity/`, and pipes of `Response`
bodies are in `body/`.
