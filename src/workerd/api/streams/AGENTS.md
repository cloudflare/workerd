# src/workerd/api/streams/

Web Streams API: ReadableStream, WritableStream, TransformStream. **Read `README.md` for full architecture** (842 lines covering data flow, pipe loops, memory safety patterns).

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
