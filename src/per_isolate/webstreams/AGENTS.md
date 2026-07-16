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
| `streams.ts`  | Module aggregator and temporary native-source exports                                        |
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
- `kNativeSource` is TEMPORARILY re-exported via `streams.ts` for tests;
  the real C++ handshake has landed, so this removal is now due
  (follow-up; requires migrating the JS-mock tests off it).

## ANTI-PATTERNS

- **NEVER** expose internals on user-visible exports (the temporary
  `kNativeSource` exception is tracked for removal).
- **NEVER** rely on `readable-source-adapter.h` as a reference for the
  native streams contract — it belongs to the original (non-enabled)
  implementation; `native.ts` is authoritative.
