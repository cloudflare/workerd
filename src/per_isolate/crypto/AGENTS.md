# src/per_isolate/crypto/

TypeScript reimplementations of crypto APIs that must be replaced when the
`typescript_implemented_streams` compat flag is on. Parent directory
conventions (primordials discipline, private-brand dispatch, no `instanceof`)
apply — see `src/per_isolate/AGENTS.md`.

## WHY THIS EXISTS

`DigestStream` is one of only two `WritableStream` subclasses in the runtime.
When the streams flag swaps `globalThis.WritableStream` for the TypeScript
class, a C++ subclass of the *C++* `WritableStream` no longer passes the
brand checks used by `pipeTo`, and `instanceof WritableStream` becomes false.
Reimplementing the subclass in TypeScript is what restores the hierarchy.

The other subclass, `FileSystemWritableFileStream`
(`src/workerd/api/filesystem.h`), has the same problem but is not addressed
here: it is constructed from C++ rather than by user code, so it needs a
native source/sink bridge instead of a straight port.

## FILE MAP

| File               | Role                                                          |
| ------------------ | ------------------------------------------------------------- |
| `digest-stream.ts` | `DigestStream` — WritableStream subclass over a native digest |

## THE C++ CONTRACT

`digest-stream.ts` owns only the stream semantics. Hashing is done by a native
`DigestContextHandle` (`src/workerd/api/crypto/crypto.h`), reached through
`utils.createDigestContext(name)`, which is registered in
`src/workerd/io/per-isolate-bootstrap.c++` via the deliberately crypto-free
declaration in `src/workerd/api/crypto/digest-bootstrap.h`.

Two parts of that contract are easy to break:

- **`update()` takes strings, and returns a byte count.** Strings are NOT
  encoded on the TypeScript side, because the default encoding is not
  expressible in JavaScript: a lone surrogate is hashed as WTF-8 (`ED A0 80`),
  and `TextEncoder` can only produce the U+FFFD substitution. The return value
  exists because a string's UTF-8 length is not observable from JavaScript.
- **The string encoding is fixed at context creation**, by
  `createDigestContext(name, toWellFormed)`. `toWellFormed: true` opts into the
  U+FFFD substitution; the default is WTF-8 and must stay that way for
  backwards compatibility. `crypto-streams-test.js` pins both directions for
  both implementations — `stringChunksAreNotTextEncodedByDefault` catches a
  changed default, `toWellFormedMatchesTextEncoder` catches an ignored option.
- **`toWellFormed` is a streaming encoder, so it is stateful.** A surrogate pair
  can be split across two writes, so a lead surrogate ending a chunk is held
  back until the next chunk decides whether it pairs. Consequences for the
  contract: `update()` can return 0 for a non-empty chunk, and **`flush()` must
  be called before `digest()`** to account for a lead that was never paired
  (3 bytes, U+FFFD). `digest()` flushes internally too, so forgetting
  `flush()` costs an accurate `bytesWritten`, never a correct digest. The
  default WTF-8 encoding is stateless and joins nothing — that is the historical
  behavior, so the two encodings can disagree on `bytesWritten` across chunks
  (4 bytes for a joined pair vs 3+3 for two lone surrogates).
- **The option bag is coerced, not type-checked.** JSG unwraps
  `jsg::Optional<bool>` with ToBoolean, so `{toWellFormed: 'false'}` opts *in*.
  The TypeScript side reproduces this with `!!`, and
  `toWellFormedIsCoerced`/`toWellFormedDefaultsToFalse` hold the two together.
- **Chunks are not copied.** The digest consumes bytes synchronously inside
  `update()`, so a copy would be pure overhead — unlike
  `webstreams/identity.ts`, which copies because it enqueues. The consequence
  is that SharedArrayBuffer-backed views are read in place.

## PARITY

Both implementations run `crypto-streams-test.js` unmodified, under
`crypto-streams-test.wd-test` (C++) and `crypto-streams-ts-test.wd-test`
(TypeScript). Behavior shared between them belongs in that file so drift fails
one config or the other. TypeScript-only behavior — and the one intentional
divergence below — belongs in `ts-digest-stream-test.js`.

`ts-digest-stream-test.js`'s `isRealWritableStreamSubclass` is the canary that
the `main.ts` install actually took effect; without it the rest of the suite
would pass against the C++ implementation too.

## INTENTIONAL DIVERGENCE

The digest promise is marked handled (`utils.markPromiseHandled`), so
abandoning it produces no unhandled-rejection report. The C++ implementation
does report in that case. This is accepted: consuming `digest` is optional, an
abort is already surfaced through the writer's `ready`/`closed` promises, and
marking does not propagate to derived promises — `stream.digest.then(f)` with
no `catch` still reports.

## ANTI-PATTERNS

- **NEVER** encode string chunks here; forward them to `update()`.
- **NEVER** reference `this` in the sink closures passed to `super()` — they
  are evaluated before `super()` returns, while `this` is still in its
  temporal dead zone. Build the state object first and capture that.
