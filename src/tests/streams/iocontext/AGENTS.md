# Streams at global scope (the IoContext boundary)

Module evaluation runs OUTSIDE any IoContext, and streams constructed
there must work — including when later used inside requests. Migrated
wholesale from streams-iocontext-test.js (itself ported from the
edgeworker streams-iocontext.ew-test), plus the module-scope
WritableStream pin from streams-test.js and new byte-stream and
TransformStream coverage. **The tests are the normative artifact.**

Structure note: the module-scope streams live in
global-scope-streams.js together with the routing fetch handler;
main.js re-exports both. Moving the constructions would change which
module's evaluation performs them.

## Coverage

| Shape | Pinned in |
| --- | --- |
| module-scope value stream as a response body | `globalScopeReadablestream` |
| module-scope stream CONSUMED at module scope (for-await during evaluation) | `globalScopeReadablestream2` |
| module-scope stream consumed inside a later request | `globalScopeReadablestream3` |
| module-scope stream piped through a request-created TransformStream | `globalScopeReadablestream4` |
| module-scope stream wrapped in a module-scope Response | `globalScopeReadablestream5` |
| module-scope stream fed by a controller stashed on globalThis, produced in one request and consumed in another | `globalScopeReadablestream6` |
| module-scope pull deferred behind a promise resolved by a later request | `globalScopeReadablestream7` |
| module-scope start() parked on a promise resolved by a later request | `globalScopeReadablestream8` |
| module-scope WritableStream (controller AbortSignal allocation needs no IoContext) | `globalScopeWritablestream` |
| module-scope BYTE stream, BYOB-read inside a request | `globalScopeByteReadable` |
| module-scope TransformStream | `globalScopeTransformStream` — **C++ CRASH BUG**: the mere existence of a module-scope `new TransformStream()` SEGFAULTS the C++ implementation when the first request enters the worker, even if never touched again; construction is which-impl-guarded and the TS side pins construction + a request-time pipe. TODO(bug): unguard once fixed |

Cross-request state (tests 6-8) uses `ctx`-less module globals on
purpose: the point is that the stream machinery itself must not capture
the constructing (nonexistent) or first-using IoContext.
