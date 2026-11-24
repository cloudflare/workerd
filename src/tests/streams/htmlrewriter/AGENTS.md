# HTMLRewriter × streams

HTMLRewriter consuming stream bodies, producing a stream body, and
reading streamed replacement content — under both stream
implementations, each with both HTMLRewriter implementations: the C++
one (`src/workerd/api/html-rewriter.{h,c++}`) and the TypeScript one
(`src/per_isolate/htmlrewriter/`, over the Rust crate in
`src/workerd/api/html-rewriter/`, under the `TS_HTML_REWRITER`
autogate). **The tests are the normative artifact.** The general
rewriter surface (selectors, handler types, comments/doctype/text
tokens, async handlers) is owned by `src/workerd/api/tests/
htmlrewriter-test.js`; this suite owns the STREAMS interaction only.

Unlike the api/tests rewriter files (which pin the pre-fixup behavior
via `original-transform-stream-backpressure`), the cpp cell here runs
under the MODERN `fixup-transform-stream-backpressure`.

## Cells

| Cell | Streams | HTMLRewriter |
| --- | --- | --- |
| `htmlrewriter-cpp` | C++ | C++ |
| `htmlrewriter-ts` | TypeScript | C++ |
| `htmlrewriter-cpp-ts-rewriter` | C++ | TypeScript |
| `htmlrewriter-ts-ts-rewriter` | TypeScript | TypeScript |

User code can't read the autogate, so each cell says which HTMLRewriter
it runs through its `USING_TS_REWRITER` binding (`which-impl.js`), for a
test that has to pin a difference between them. The
C++ HTMLRewriter cells disable `@all-autogates`, which would swap in the
TypeScript one.

## Coverage

| Test | Shape |
| --- | --- |
| `passthroughJsValueStream` / `passthroughJsByteStream` | no-handler passthrough over JS value/byte stream bodies |
| `handlerAcrossChunkBoundaries` | chunks split MID-TAG; the parser reassembles and the handler mutates both elements |
| `rewrittenBodyIsReadableStream` | output body drained incrementally via a reader |
| `contentFromReadableStream` | element.replace(ReadableStream) — streamed replacement content |
| `stringChunksAreUtf8` | string chunks in the transformed body and in streamed replacement content read as UTF-8 beside byte chunks (parity) |
| `identityStreamBody` | identity body fed by a concurrent writer |
| `cancelReachesSourceAfterNextChunk` | cancel remains pending at the source until another chunk wakes the pump, then reaches the source |
| `erroringSourceRejectsConsumption` | source error surfaces from .text() |
| `largeDocumentThroughHandler` | 1024 elements / ~264 KiB through a counting handler, byte-exact output |
| `outputBeforeAWaitingHandler` | the output before an element whose handler waits on a promise, read while it waits |

## Divergences

### Streams implementations (C++ HTMLRewriter)

After the next source chunk wakes a canceled rewriter pump, C++ makes one
additional pull while TypeScript makes two; these counts are pinned by
`cancelReachesSourceAfterNextChunk`.

### HTMLRewriter implementations

The TypeScript one transforms through the same native parts as the C++
one (an identity pipe for the transformed body, and the native pump for
the original body and streamed content), so reads, cancellation, error
conversion and pull counts match. It writes the output before a handler
that returns a promise as the handler starts, where the C++ one writes it
once the handler is done: `outputBeforeAWaitingHandler` reads it while the
handler waits with the TypeScript one only. The header of
`src/per_isolate/htmlrewriter/htmlrewriter.ts` lists the other
differences, none of which shows here.

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
