# src/workerd/api/streams/

Web Streams API: ReadableStream, WritableStream, TransformStream.
See `README.md` for terse reference (classification, state machines, safety patterns).
See `docs/streams.md` for narrative tutorial.

## ARCHITECTURE

Dual implementation behind unified API:

- **Internal** (`internal.h`): kj-backed, byte-only, single pending read, no JS queue
- **Standard** (`standard.h`): WHATWG spec, JS promises, value+byte, dual queues (data + pending reads)

Controller pattern: `ReadableStream` → `ReadableStreamController` → impl-specific controller.
4 pipe loop variants (kj↔kj, kj↔JS, JS↔kj, JS↔JS) selected by stream type combination.
Tee uses `kj::Rc<Entry>` shared refs (non-standard optimization, avoids data copies).

Key deps: `util/state-machine.h` (stream state FSM), `util/weak-refs.h`, `util/ring-buffer.h`.

## FILE MAP

| File                                | Role                                                                                                |
| ----------------------------------- | --------------------------------------------------------------------------------------------------- |
| `readable.{h,c++}`                  | `ReadableStream` public API, `ReaderImpl`, readers                                                  |
| `writable.{h,c++}`                  | `WritableStream` public API, `WriterImpl`, writer                                                   |
| `transform.{h,c++}`                 | Standard `TransformStream` (JS algorithms)                                                          |
| `identity-transform-stream.{h,c++}` | Internal identity transform (byte-only, 1:1 read↔write)                                             |
| `internal.{h,c++}`                  | `ReadableStreamInternalController`, `WritableStreamInternalController`                              |
| `standard.{h,c++}`                  | `ReadableStreamJsController`, `WritableStreamJsController` — main complexity (~5400 lines combined) |
| `readable-source.{h,c++}`           | `ReadableStreamSource` — kj `AsyncInputStream` adapter                                              |
| `writable-sink.{h,c++}`             | `WritableStreamSink` — kj `AsyncOutputStream` adapter                                               |
| `readable-source-adapter.{h,c++}`   | Standard→Internal bridge (wraps JS controller as `ReadableStreamSource`)                            |
| `writable-sink-adapter.{h,c++}`     | Standard→Internal bridge (wraps JS controller as `WritableStreamSink`)                              |
| `common.{h,c++}`                    | Shared types: `ReadResult`, `StreamStates`, controller interfaces                                   |
| `queue.{h,c++}`                     | `ValueQueue`/`ByteQueue` for Standard stream backpressure                                           |
| `compression.{h,c++}`               | `CompressionStream`/`DecompressionStream` (zlib transforms)                                         |
| `encoding.{h,c++}`                  | `TextEncoderStream`/`TextDecoderStream`                                                             |

## INVARIANTS

These rules MUST be followed when modifying stream code:

1. **Always use `deferControllerStateChange()`** when calling code that may invoke JS
   callbacks during a read/write operation
2. **Always use `snapshot()`** when iterating over consumers if loop body may trigger JS
3. **Never access `this`** after calling a StateListener callback that may destroy the object
4. **Always re-check lock state** in lambda continuations that may execute after lock release
5. **Use WeakRef** for any handle that user code may hold longer than the underlying object
6. **State before promises**: Transition state before resolving promises; pop queue entries
   only after resolving/rejecting their associated promises
7. **No dangling captures**: Prefer capturing `this` + re-acquiring refs inside lambdas
   over capturing raw references that may become dangling

See `README.md` §Safety Pattern Catalog for detailed When/Why/How for each pattern.

## CODE REVIEW RULE

When reviewing changes to streams code, check whether the change requires updates to
any of these documentation files:

- **`README.md`** — if the change alters stream classification (e.g. new stream type),
  state machine behavior, pipe loop selection, or introduces/modifies a safety pattern
- **`docs/streams.md`** — if the change affects user-visible behavior, data flow
  semantics, or backpressure mechanics that the tutorial describes
- **This file (`AGENTS.md`)** — if the change adds/removes files, changes the
  architecture summary, or introduces new invariants

Flag any needed doc updates in the review. Do not let behavioral or architectural
changes land without corresponding documentation updates.
