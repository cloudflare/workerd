# TextEncoderStream and TextDecoderStream

An informal specification of the two WHATWG Encoding stream classes as
implemented in workerd, derived from — and kept in lockstep with — the test
suite in this directory. **The tests are the normative artifact**; this
document indexes every specified behavior to the test that asserts it. Both
the C++ implementation (`src/workerd/api/streams/encoding.{h,c++}`) and the
TypeScript implementation (`src/per_isolate/webstreams/encoding.ts`, behind
`typescript_implemented_streams`) are covered.

The WPT `encoding/streams/*` tests already run against both implementations
(`//src/wpt:encoding` and `//src/wpt:encoding-ts`), so this suite
**complements** WPT rather than re-asserting it: it pins workerd-specific
behaviors WPT cannot see (the WPT harness forces `pedantic_wpt`), exact
error identities and messages, the implementation divergences, the
compatibility-flag axes, and the pre-flag legacy behaviors.

## Interfaces

```webidl
[Exposed=Worker]
interface TextEncoderStream {
  constructor();
  readonly attribute DOMString encoding;  // always "utf-8"
  readonly attribute ReadableStream readable;
  readonly attribute WritableStream writable;
};

[Exposed=Worker]
interface TextDecoderStream {
  constructor(optional DOMString label = "utf-8",
              optional TextDecoderOptions options = {});
  readonly attribute DOMString encoding;
  readonly attribute boolean fatal;
  readonly attribute boolean ignoreBOM;
  readonly attribute ReadableStream readable;
  readonly attribute WritableStream writable;
};
```

Interface notes:

- **Inheritance divergence (ledger #2):** in C++ both classes extend
  `TransformStream`; in TypeScript they are standalone (the
  `CompressionStream`/`IdentityTransformStream` convention).
- `encoding`/`fatal`/`ignoreBOM` are own enumerable get-accessors on the
  class prototype in both implementations. `readable`/`writable` placement
  diverges (ledger #3). Instances carry no own properties. All getters are
  brand-checked (`TypeError`, message prefix `Illegal invocation`).
- Instances are branded via `Symbol.toStringTag`
  (`[object TextEncoderStream]` / `[object TextDecoderStream]`).
- Both classes wrap the C++ codec primitives: the TypeScript streams create
  a real `TextDecoder`/`TextEncoder`, so label handling and decode results
  are shared by construction.

## Core semantics

These are standard TransformStreams (writes settle when the transform
consumes the chunk — not the identity streams' rendezvous), with writable
HWM 1 (count-based) and readable HWM 0 under
`encoder_stream_spec_compliant_backpressure`:

- **No transform without demand:** with readable HWM 0, no write — not even
  the first — settles until a read creates pull demand.
- `desiredSize` counts queued chunks against the writable HWM of 1;
  `writer.ready` is replaced under backpressure and the same promise
  resolves when reads drain the queue.

### TextEncoderStream

- Chunks are ToString-coerced (`undefined` → `"undefined"`, objects via
  their own `toString`); symbols throw `TypeError` and error the stream.
- Surrogate state spans chunks: a lone trailing high surrogate is held and
  paired with the next chunk's leading low surrogate; unpairable surrogates
  become U+FFFD; a pending high surrogate at close is flushed as U+FFFD.
  The held unit is prepended to the next chunk's text, so the pair (or
  replacement) arrives inside that chunk's single enqueue.
- Empty-string writes resolve without enqueuing.

### TextDecoderStream

- Accepts `BufferSource` chunks only (any view type, honoring offsets);
  anything else rejects with `TypeError` (message per ledger #4) and errors
  the stream: `closed`, pending reads, and later writes all reject with the
  same error. An already-detached `ArrayBuffer` decodes as zero bytes (a
  no-op).
- There is **no write-time snapshot** (contrast the identity streams): the
  chunk is held by reference and the transform reads the buffer's CURRENT
  bytes when it runs. Mutating or shrinking after `write()` changes what
  decodes; detaching while queued makes the chunk contribute nothing.
  Shadowing metadata getters are never consulted (internal slots only).
- Decode state spans chunks (`stream: true`); `close()` runs a final flush
  decode that emits U+FFFD for an incomplete trailing sequence (or throws
  in fatal mode, rejecting the close and erroring the readable). A BOM
  split across chunks is still stripped; `ignoreBOM: true` preserves it.
  Empty decode outputs are never enqueued.
- Fatal-mode failures reject the offending write/close and both `closed`
  promises with `TypeError` "Failed to decode input.".
- The label selects the codec (`big5` etc. under `text_decoder_cjk_decoder`),
  with ASCII-whitespace trimming, lowercasing, and alias resolution
  (`'utf-16'` → `'utf-16le'`, `'L1'` → `'windows-1252'`); invalid labels
  throw `RangeError` `"<label>" is not a valid encoding.`.

### Propagation and composition

- `writer.close()` resolves pending reads as done and settles both `closed`
  promises. `readable.cancel(reason)` and `writable.abort(reason)` carry
  the **original reason instance** to the far side (standard JS-controller
  streams in both implementations — contrast identity suite ledger #8).
- `pipeThrough` chains compose; `tee()` on the decoder's readable delivers
  to both branches with single-branch demand driving the writer.

### Bodies

- The encoder's readable is a byte source: `new Response(tes.readable)` /
  `new Request(url, {method, body: tes.readable})` neither wrap, consume,
  nor lock it (`resp.body` is the same object), and `text()` drives the
  transform — including large payloads chunked at surrogate-splitting
  boundaries.
- The decoder's readable yields **strings**: consuming it as a body rejects
  with `TypeError` "This ReadableStream did not return bytes." while the
  write settles normally; under `pedantic_wpt` the C++ failing consumer's
  cancel propagates to the writable and `close()`/`closed` reject with the
  same error. `response.body.pipeThrough(tds)` is the working direction.
- Piping a byte body **into** the encoder's writable is a footgun, not an
  error: each `Uint8Array` chunk is ToString-coerced (`"120,121"`) and that
  text is encoded.

### Re-entrancy and GC

- Two user hooks run mid-processing: the thenable check against read
  results (a patched `Object.prototype.then` getter fires per read —
  counts per ledger #6) and the encoder's ToString coercion (a user
  `toString()` runs inside the transform). Re-entering the stream from
  either — closing the writer, issuing further writes — is safe: the
  re-entrant write queues behind the chunk being processed and everything
  settles in order.
- These are standard readables in both implementations: a second
  concurrent default read parks and is served in order (contrast identity
  ledger #16).
- Reader/writer handles keep a collected stream wrapper's machinery —
  including the C++ decoder ref — alive and operable (`--expose-gc` in
  both configs).

## Compatibility flags

The C++ cell pins every date-gated flag the implementation is subject to;
`encoding-ts.wd-test` pins only `text_decoder_cjk_decoder` (the shared codec
primitive) and proves streams-flag indifference via its variants.

| Flag (enable date) | Selects | Unflagged behavior tested by |
| --- | --- | --- |
| `streams_enable_constructors` + `transformstream_enable_standard_constructor` (2022-11-30) | the standard TransformStream machinery running the codec transformer | `legacy-identity-fallback.js` |
| `fixup-transform-stream-backpressure` (2024-12-16) | effective TransformStream backpressure | `legacyAllWritesSettleWithoutDemand` |
| `encoder_stream_spec_compliant_backpressure` (2026-03-24) | readable-side HWM 0 | `legacyFirstWriteSettlesEagerly` |
| `text_decoder_cjk_decoder` (2026-03-03) | dedicated CJK decoder for non-UTF-8 labels (codec primitive, both cells) | — |
| `workers_api_getters_setters_on_prototype` (2022-01-31), `set_tostring_tag` (2024-09-26) | prototype accessors / branding (pinned; generic JSG behaviors, unflagged sides guarded by the identity suite's legacy cell) | — |

`pedantic_wpt` (opt-in only, no date) aligns C++ behaviors with the spec
where the production default deviates, and is consulted throughout the
standard-streams machinery these classes are built on.
`encoding-cpp-pedantic.wd-test` runs the FULL shared module set with it
added to the main cell's pinned flags; on this suite's surface it changes
exactly two things, both pinned:

- the TDS fatal default with an options bag lacking `fatal` becomes the
  spec's `false` (ledger #1; `fatalDefaults` asserts per mode, the
  unflagged side is also pinned by `legacyFatalDefaultsTrueWithOptionsBag`)
- a failing body consumer's cancel propagates to the writable, so
  `close()`/`closed` reject with the consumer's TypeError instead of
  resolving (`decoderReadableAsBodyRejectsText`)

Production workers never get `pedantic_wpt`, so the main cell continues to
assert the defaults.

`encoding-ts.wd-test` additionally sets the internal-testing
`expose_draining_reader` flag, installing the `ReadableStreamDrainingReader`
global — the bulk-drain conduit the C++ bridge drives to consume TypeScript
streams. No such global exists under the C++ implementation;
`draining-reader.js` asserts both sides.

## Divergence ledger (C++ vs TypeScript)

Every entry is asserted on both sides via the `which-impl` pattern.

| # | Area | C++ | TypeScript | Pinned in |
| --- | --- | --- | --- | --- |
| 1 | TDS `fatal` default with an options bag lacking `fatal` | `true` (spec's `false` only under `pedantic_wpt`) | `false` (spec) | `fatalDefaults` (per mode), `legacyFatalDefaultsTrueWithOptionsBag` |
| 2 | `TransformStream` inheritance | subclass; `instanceof` true | standalone | `transformStreamInheritance` |
| 3 | `readable`/`writable` placement | inherited from `TransformStream.prototype` | own enumerable accessors on the class prototype | `accessorPlacement` |
| 4 | Invalid TDS chunk `TypeError` message | "This TransformStream is being used as a byte stream, but received a value that is not a BufferSource." | "TextDecoderStream: chunk must be a BufferSource" | `decoderRejectsNonBufferSource` |
| 5 | Constructor source text | native code | not | `constructorSurface` |
| 6 | Thenable check during read resolution | `Object.prototype.then` getter consulted once per read | twice | `thenInterceptionDuringReadResolution` |
| 7 | In-flight write rejection when the readable is cancelled mid-transform | `TypeError` "The readable side of this TransformStream is no longer readable." | `TypeError` "Cannot enqueue a chunk into a stream that is closed or has been errored" | `cancelReadableFromChunkToString` |

## Assertion catalogue

| Module | Asserts |
| --- | --- |
| `api-surface.js` | encoding getter; toStringTag branding; inheritance (ledger #2); accessor placement (ledger #3); side stability + no own instance props; getter brand checks; `node:stream/web` re-exports are the same classes; constructor name/length/source (ledger #5) |
| `construction.js` | option reflection + `utf-16` alias; fatal defaults (ledger #1) incl. explicit values and ignoreBOM default; invalid label `RangeError` with exact message; label whitespace/case/alias normalization |
| `chunk-types.js` | TDS BufferSource acceptance across view types with offsets; detached-buffer no-op; non-BufferSource rejection (ledger #4) with errored-stream aftermath; TES symbol chunk `TypeError` errors the stream |
| `encode-coercion.js` | ToString coercion of undefined/number/object chunks |
| `encode-surrogates.js` | pair split across writes → single astral chunk; lone high + BMP → replacement-plus-char in one chunk; lone low → replacement; pending high at close → flush replacement |
| `decode-splits.js` | split BOM stripped by default; `ignoreBOM` preserves it; incomplete sequence at close → replacement from the flush |
| `decode-non-utf8.js` | big5 label selects the codec; decode state carries across byte-at-a-time writes |
| `fatal-mode.js` | invalid bytes reject write/read/both `closed` with "Failed to decode input."; incomplete sequence at close rejects `close()` and the pending read |
| `zero-length-writes.js` | empty string / empty view / empty buffer are non-delivering no-ops |
| `backpressure.js` | `desiredSize` counts queued chunks (writable HWM 1); no write settles without read demand (readable HWM 0); `ready` replacement and same-promise recovery |
| `propagation.js` | close resolves pending read + `closed` promises; cancel/abort reasons cross as the original instance, incl. later writes |
| `pipe-integration.js` | encoder → decoder → encoder `pipeThrough` chain |
| `tee.js` | both branches observe content; single-branch demand drives the writer; EOF on both |
| `body-integration.js` | Response/Request with the encoder's readable as body (same object, unlocked; `text()` incl. a large surrogate-split payload); the decoder's string-yielding readable as body rejects with "This ReadableStream did not return bytes." (writer side settles); `response.body.pipeThrough(tds)` decodes; byte body piped into the encoder ToString-coerces the chunks |
| `buffer-lifecycle.js` | no write-time snapshot: mutation after `write()` is visible (with and without a parked read); a shrunk length-tracking view decodes its remaining bytes; detach-while-queued contributes nothing; shadowing/throwing metadata getters never consulted |
| `reentrancy.js` | thenable-check interception counts (ledger #6); re-entrant `writer.close()` from the interceptor mid-delivery; the encoder's `toString()` hook re-entering with write+close (delivery order preserved, clean EOF); `readable.cancel()` from the hook (the AUTOVULN-63 trigger shape — ledger #7; the C++ use-after-free regression lives in api/tests/autovuln-63-test.js); write from a read continuation; second concurrent read parks and is served in order (parity — contrast identity #16); sibling tee-branch cancel from a read continuation |
| `draining-reader.js` | TS only (C++ cell asserts the global's absence): `expectedLength` undefined for both streams; encoder drains as one `Uint8Array` chunk per read (HWM 0 — nothing synchronously buffered) with EOF as a separate empty batch; a tee-sibling backlog IS swept in one batched read; decoder chunks pass through the conduit as raw strings (byte validation happens at consumption) |
| `gc-interplay.js` | a writer keeps its collected encoder wrapper operable (abort); reader+writer keep a collected decoder wrapper decoding through close (`--expose-gc`) |
| `which-impl.js` | implementation detection |

## Legacy (pre-flag) behaviors

C++ only; one cell per flag window, each with
`generate_all_compat_flags_variant = False`.

| Cell | Window | Behavior | Asserted by |
| --- | --- | --- | --- |
| `encoding-cpp-legacy.wd-test` | pre-2022-11-30 (fully unflagged) | the codec transformer is dropped: both classes are identity streams. TES still UTF-8-encodes strings (identity encodes string writes) but with no cross-chunk surrogate pairing, and its readable supports BYOB; TDS passes bytes through undecoded; invalid chunks throw synchronously (no `capture_async_api_throws`) and the stream survives; option getters still reflect the real decoder, including the non-pedantic fatal quirk (options bag lacking `fatal` → `true`) | `legacy-identity-fallback.js` |
| `encoding-cpp-legacy-bp.wd-test` | 2022-11-30..2024-12-16 | real codec, but no effective TransformStream backpressure: every write settles without read demand | `legacyAllWritesSettleWithoutDemand` |
| `encoding-cpp-legacy-hwm.wd-test` | 2024-12-16..2026-03-24 | fixed backpressure with readable HWM 1: the first write settles without demand, later writes park | `legacyFirstWriteSettlesEagerly` |

Generic pre-flag JSG behaviors (own-instance property placement, `[object
Object]` stringification) are guarded by the identity suite's legacy cell
and not repeated here.
