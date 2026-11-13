# src/per_isolate/crypto/

TypeScript reimplementations of crypto APIs: `DigestStream`, which must be
replaced when the `typescript_implemented_streams` compat flag is on, and
`SubtleCrypto`, under the `TS_WEBCRYPTO` autogate. Parent directory
conventions (primordials discipline, private-brand dispatch, no `instanceof`)
apply — see `src/per_isolate/AGENTS.md`.

## WHY THIS EXISTS

`DigestStream` is one of only two `WritableStream` subclasses in the runtime.
When the streams flag swaps `globalThis.WritableStream` for the TypeScript
class, a C++ subclass of the _C++_ `WritableStream` no longer passes the
brand checks used by `pipeTo`, and `instanceof WritableStream` becomes false.
Reimplementing the subclass in TypeScript is what restores the hierarchy.

The other subclass, `FileSystemWritableFileStream`, is fixed the same way in
`src/per_isolate/webfs/`. Its construction runs the other direction — instances
come from `FileSystemFileHandle.createWritable()` rather than a user-callable
constructor — so that method is replaced on the prototype as well.

## FILE MAP

| File                    | Role                                                          |
| ----------------------- | ------------------------------------------------------------- |
| `digest-stream.ts`      | `DigestStream` — WritableStream subclass over a native digest |
| `subtle.ts`             | `SubtleCrypto` — the interface over the native operations     |
| `subtle-conversions.ts` | JSG's conversions of SubtleCrypto's dictionaries and members  |

## THE C++ CONTRACT

`digest-stream.ts` owns only the stream semantics. Hashing is done by a native
`DigestContextHandle` (`src/workerd/api/crypto/crypto.h`), reached through
`utils.createDigestContext(name)`, which is registered in
`src/workerd/api/per-isolate-bootstrap.c++` via the deliberately crypto-free
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
  `jsg::Optional<bool>` with ToBoolean, so `{toWellFormed: 'false'}` opts _in_.
  The TypeScript side reproduces this with `!!`, and
  `toWellFormedIsCoerced`/`toWellFormedDefaultsToFalse` hold the two together.
- **Chunks are not copied.** The digest consumes bytes synchronously inside
  `update()`, so a copy would be pure overhead — unlike
  `webstreams/identity.ts`, which copies because it enqueues. The consequence
  is that SharedArrayBuffer-backed views are read in place.

## SUBTLECRYPTO

`subtle.ts` owns the WebCrypto interface: the class, the receiver check,
argument conversion order and messages, and whether a failure throws or
rejects. The operations, and `CryptoKey`, stay native: `SubtleCryptoBackend`
(`src/workerd/api/crypto/crypto.h`), from `utils.createSubtleCryptoBackend()`
(declared crypto-free in `src/workerd/api/crypto/subtle-bootstrap.h`), runs
the same `SubtleCrypto::*Sync()` bodies the C++ methods run.

- **Each argument is converted as JSG converts it.** `subtle-conversions.ts`
  holds a table per `JSG_STRUCT`, in the C++ declaration order, and coerces
  each member as its C++ type is. The backend supplies only the checks JS
  cannot make (`isCryptoKey`); buffers and arrays are tested with `utils`. Each
  result is an object literal with every member, so the backend's second
  conversion never reads a prototype; a table must list every member of its
  struct.
- **The backend is created on first use**, so that a context that never
  calls SubtleCrypto does not instantiate its class.
- **A missing argument is not `undefined`.** JSG rejects a required parameter
  past `arguments.length`, even of a type that accepts `undefined`; hence the
  rest parameters.
- **Throw or reject follows `capture_async_api_throws`.** Without it, a failed
  conversion throws; the operation always rejects; a bad receiver always
  throws.
- **`SubtleCrypto` is a function, not a class**, so that calling it without
  `new` throws `Illegal constructor` as JSG does.
- `crypto.subtle` stays the C++ `Crypto` property: `Crypto::getSubtleForJs()` reads
  the instance `main.ts` stores under the global's V8-private
  `workerd:SubtleCrypto`.

`src/workerd/api/tests/crypto-subtle-parity-test.js` records every observable
behaviour of the C++ class, at the oldest date and with every flag
(`crypto-subtle-parity-expected.js`), and runs against both implementations
(`crypto-subtle-parity-test.wd-test`, `crypto-subtle-parity-ts-test.wd-test`).
The WPT suite runs against both too (`WebCryptoAPI`, `WebCryptoAPI-ts`).

## DIGESTSTREAM PARITY

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
