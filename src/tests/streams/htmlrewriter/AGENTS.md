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

## Coverage (parity — no divergences observed)

| Test | Shape |
| --- | --- |
| `passthroughJsValueStream` / `passthroughJsByteStream` | no-handler passthrough over JS value/byte stream bodies |
| `handlerAcrossChunkBoundaries` | chunks split MID-TAG; the parser reassembles and the handler mutates both elements |
| `rewrittenBodyIsReadableStream` | output body drained incrementally via a reader |
| `contentFromReadableStream` | element.replace(ReadableStream) — streamed replacement content |
| `identityStreamBody` | identity body fed by a concurrent writer |
| `cancelDoesNotReachSource` | PARITY PIN: cancelling the transformed body does NOT invoke the source's cancel hook (contrast pipeTo); demand simply stops (bounded) |
| `erroringSourceRejectsConsumption` | source error surfaces from .text() |
| `largeDocumentThroughHandler` | 1024 elements / ~264 KiB through a counting handler, byte-exact output |

The api/tests htmlrewriter-transform-cancel-test.js (cancel-before-read
×50 UAF regression) stays where it is, per the security-regression
policy.
