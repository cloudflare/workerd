# Encoding stream benchmarks

`TextEncoderStream` and `TextDecoderStream`, under both implementations.
Suite rules: `src/bench/streams/AGENTS.md`.

The shapes are `256 x 16`, `64 x 4 Ki` and `16 x 64 Ki` characters or
bytes (`SHAPES` in `helpers.js`): a stream of short chunks, like generated
tokens, and longer ones. `TEXT` names the text: ASCII, and two- and
three-byte characters in UTF-8.

## Module map

| Module | Group | Cases |
| --- | --- | --- |
| `construction.js` | `construction` | 16 constructions per call: `TextEncoderStream`; `TextDecoderStream`; `TextDecoderStream` with `fatal` |
| `encoder.js` | `encoder` | Every shape of every text, written as strings and read back as bytes |
| `decoder.js` | `decoder` | Every shape of every text, written as bytes and read back as text, and ASCII with `fatal` |
| `pipeline.js` | `pipeline` | ASCII strings through `TextEncoderStream` and back through `TextDecoderStream` with `pipeThrough()`, in every shape |

## Comparing

- Both `TextDecoderStream`s decode with `TextDecoder`'s native decoder, so
  the `decoder` differences are in the streams around it.
- The `TextEncoderStream`s encode differently. The TypeScript one calls
  `TextEncoder.prototype.encode()` on each chunk, which writes UTF-8
  through V8, or with simdutf under the
  `workerd-autogate-enable-fast-textencoder` autogate (`@all-autogates`);
  the C++ one converts each chunk with simdutf itself. The `encoder`
  differences therefore include the encoders'.
- The decoder's chunks are consecutive pieces of one encoded text. No chunk
  size is a multiple of three, so the three-byte chunks end mid-character
  and the decoder carries the partial character over to the next chunk.
