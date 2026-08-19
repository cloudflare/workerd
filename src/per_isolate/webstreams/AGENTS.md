# src/per_isolate/webstreams/

TypeScript Streams implementation with a backend-blind reader layer and
two consumer backends behind the `StreamConsumer`/`ByteStreamConsumer`
fence. Authoritative docs are IN-SOURCE — read the file headers first.

Parent directory conventions (primordials discipline, JSG capture trap,
private-brand dispatch, no `instanceof`) apply here — see
`src/per_isolate/AGENTS.md`.

## FILE MAP

| File          | Role                                                                                         |
| ------------- | -------------------------------------------------------------------------------------------- |
| `queue.ts`    | QUEUED backend: single-queue/multi-cursor, JS sources; fence interfaces; invariant list      |
| `native.ts`   | NATIVE backend: C++-backed pull conduit; **the C++/JS contract** + invariants                |
| `readable.ts` | Reader layer + queued controllers + the BACKEND-DISPATCH points (constructor, tee, chains, byte-capable gate, JS-to-C++ extraction) |
| `writable.ts` / `transform.ts` / `strategies.ts` | WHATWG writable/transform/strategies                                     |
| `identity.ts` | IdentityTransformStream and FixedLengthStream (byte-capable identity transforms)             |
| `encoding.ts` | TextEncoderStream and TextDecoderStream (pure JS codec transforms)                           |
| `streams.ts`  | Module aggregator (user-visible classes + the flag-gated DrainingReader)                     |
| `types.d.ts`  | TypeScript type definitions for the streams API                                              |

## KEY RULES

- The reader layer must stay backend-blind; backend divergence is
  confined to the fence interface and the marked BACKEND-DISPATCH points.
- Do not port logic across the fence without checking BOTH invariant
  lists (`queue.ts` and `native.ts` headers).
- The native source contract (marker symbol, standard pull/cancel hooks,
  byobRequest discrimination, once-per-pull delivery, per-pull abort
  signal for cancellation, under-delivery = fused
  `{done: true, value: partial}` EOF, tee hook, `expectedLength`
  exact-total byte contract) is specified in the `native.ts` header.
  The C++ implementation (`ReadableStreamNativeSource` in
  `src/workerd/api/js-readable-stream.{h,c++}`) MUST conform to it; JS
  mocks in tests exercise the conduit independently. Key addition:
  `pull` receives an extension `signal`
  argument — the source checks `signal.aborted` before delivery and stashes
  bytes for redelivery if aborted (race buffering lives source-side; the JS
  conduit is uniformly bufferless).
- `nativeStreamInternals` (markers, extraction symbols, conduit
  construction) is module-private, consumed only by readable.ts/writable.ts
  and the C++ bridge via the API-symbol registry. The C++ mocks in
  `js-readable-stream-test.c++` construct real `ReadableStreamNativeSource`
  objects; no JS-visible marker export exists.

## ANTI-PATTERNS

- **NEVER** expose internals on user-visible exports. `streams.ts` exports
  exactly the user-visible classes plus `ReadableStreamDrainingReader`,
  which `main.ts` installs only under the internal-testing
  `expose_draining_reader` flag.
