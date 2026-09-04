# HTMLRewriter × streams

HTMLRewriter consuming stream bodies, producing a stream body, and
reading streamed replacement content — under both stream
implementations. **The tests are the normative artifact.** The general
rewriter surface (selectors, handler types, comments/doctype/text
tokens, async handlers) is owned by `src/workerd/api/tests/
htmlrewriter-test.js`; this suite owns the STREAMS interaction only.

Unlike the api/tests rewriter files (which pin the pre-fixup behavior
via `original-transform-stream-backpressure`), the cpp cell here runs
under the MODERN `fixup-transform-stream-backpressure`.

## Coverage

| Test | Shape |
| --- | --- |
| `passthroughJsValueStream` / `passthroughJsByteStream` | no-handler passthrough over JS value/byte stream bodies |
| `handlerAcrossChunkBoundaries` | chunks split MID-TAG; the parser reassembles and the handler mutates both elements |
| `rewrittenBodyIsReadableStream` | output body drained incrementally via a reader |
| `contentFromReadableStream` | element.replace(ReadableStream) — streamed replacement content |
| `identityStreamBody` | identity body fed by a concurrent writer |
| `cancelReachesSourceAfterNextChunk` | cancel remains pending at the source until another chunk wakes the pump, then reaches the source |
| `erroringSourceRejectsConsumption` | source error surfaces from .text() |
| `largeDocumentThroughHandler` | 1024 elements / ~264 KiB through a counting handler, byte-exact output |

## Divergences

After the next source chunk wakes a canceled rewriter pump, C++ makes one
additional pull while TypeScript makes two; these counts are pinned by
`cancelReachesSourceAfterNextChunk`.

## Known bugs

With TypeScript streams, canceling the transformed body reports the handled
`Error: done early` through `logUncaughtException` (tail traces / inspector),
although it is not delivered to `unhandledrejection` listeners. C++ does not
report it. This is tracked in [#7239](https://github.com/cloudflare/workerd/issues/7239).

An erroring source is rewrapped by both implementations. Even though the
`.text()` rejection is handled, the source error is reported as uncaught twice
with C++ streams and once with TypeScript streams. This is tracked in
[#7240](https://github.com/cloudflare/workerd/issues/7240).

The api/tests htmlrewriter-transform-cancel-test.js (cancel-before-read
×50 UAF regression) stays where it is, per the security-regression
policy.
