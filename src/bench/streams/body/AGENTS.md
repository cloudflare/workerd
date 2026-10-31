# Body benchmarks

Where the runtime's bodies meet streams, under both implementations: the
runtime reading JavaScript streams as bodies, JavaScript reading bodies the
runtime produces, and the APIs built on bodies. Suite rules:
`src/bench/streams/AGENTS.md`.

The JavaScript stream shapes are `256 x 64 B`, `64 x 4 KiB` and
`16 x 64 KiB` (`SHAPES` in `helpers.js`). Bodies and writes copy their
bytes, so each size's buffer is shared.

The configs bind `SELF` to the worker itself. `main.js`'s default export is
`service-binding.js`'s fetch handler, which the `serviceBinding` cases call.

## Module map

| Module | Group | Cases |
| --- | --- | --- |
| `consume.js` | `consume` | `arrayBuffer()` of a `Response` over a value stream and over a byte stream, in every shape; `text()` of a value stream; `arrayBuffer()` of an `IdentityTransformStream` fed by a concurrent writer |
| `read.js` | `read` | A `Response`'s body read with a default reader (4 KiB, 64 KiB, 1 MiB), a BYOB reader into 16 KiB views (64 KiB, 1 MiB) and `for await` (1 MiB); a string body; `Blob.stream()` |
| `pipe.js` | `pipe` | A `Response`'s body (64 KiB, 1 MiB) piped into a `WritableStream`, through a `TransformStream` into a new `Response`, and straight into a new `Response`; through `TextDecoderStream` and `IdentityTransformStream` (64 KiB) |
| `clone.js` | `clone` | `clone()` and `body.tee()` of a `Response` (64 KiB, 1 MiB), both read; `clone()` of a value stream body |
| `service-binding.js` | `serviceBinding` | Through `SELF`: a `Uint8Array` (1 KiB, 64 KiB, 1 MiB), a value stream (every shape) and a byte stream POSTed and echoed back; GET responses read with a reader (64 KiB, 1 MiB); 256 short tokens streamed back through `TextEncoderStream` |
| `html-rewriter.js` | `htmlRewriter` | `HTMLRewriter` over 4 KiB and 64 KiB documents with no handlers, an element handler and a text handler; the element handler over a value stream body |
| `form-data.js` | `formData` | `formData()` parsing 16 fields, 256 fields, and 16 fields with a 256 KiB file, from a `Uint8Array` body and from a value stream in 4 KiB chunks; a `FormData` serialized into a body and parsed back |

## Comparing

- Under the TypeScript implementation, a body the runtime produces is a
  TypeScript stream over a native source, and a JavaScript stream handed to
  the runtime is read through the C++ bridge, so these cases measure the
  bridge in both directions. The implementations read bodies in different
  chunk sizes, so the cases count bytes, not reads.
- The `SELF` handler runs in the same isolate as the caller, under the same
  implementation, so a round trip measures both ends.
