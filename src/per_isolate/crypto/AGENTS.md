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
  encoded on the TypeScript side. `jsg::Lock::toString()` emits WTF-8, which
  differs from `TextEncoder` for unpaired surrogates, and both DigestStream
  implementations must agree. The return value exists because a string's UTF-8
  length is not observable from JavaScript. `crypto-streams-test.js`
  (`stringChunksAreNotTextEncoded`) pins this for both implementations.
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
