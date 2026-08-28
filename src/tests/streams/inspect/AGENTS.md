# util.inspect over streams

Informal specification of `node:util` inspect output for every stream
surface, derived from — and kept in lockstep with — this suite. **The
tests are the normative artifact.**

## The one divergence (it is the whole suite)

The C++ implementation installs a custom inspect exposing lock/state
internals; the TypeScript implementation has none — every stream
inspects as a bare `ClassName {}` regardless of state, so introspection
consumers lose `[state]`, `[supportsBYOB]`, `[length]`, and
`[expectsBytes]` under TS. Both sides are pinned verbatim at every
lifecycle transition.

| Surface | C++ shape (pinned verbatim per state) | Pinned in |
| --- | --- | --- |
| value ReadableStream | `ReadableStream { locked, [state]: 'readable'→'closed', [supportsBYOB]: false, [length]: undefined }` | `inspectValueReadable` |
| errored ReadableStream | `[state]: 'errored'` | `inspectErroredReadable` |
| byte ReadableStream | `[supportsBYOB]: true` | `inspectByteReadable` |
| WritableStream | `WritableStream { locked, [state]: 'writable'→'closed', [expectsBytes]: false }` | `inspectWritable` |
| erroring WritableStream | `[state]: 'erroring'→'errored'` (the transitional state is observable) | `inspectErroringWritable` |
| FixedLengthStream | composite readable+writable; `[length]` counts DOWN as bigint (5n→2n→0n) as reads drain | `inspectFixedLengthStream` |
| IdentityTransformStream | composite; abort drives writable `'errored'`, first failed read drives readable `'errored'` | `inspectErroredIdentityStream` |

## Compatibility flags

The C++ cell pins, beyond the constructors pair:
`internal_writable_stream_abort_clears_queue` +
`writable_stream_spec_compliant_writer` (the identity abort reaches
`'errored'` immediately only under modern abort semantics) and
`workers_api_getters_setters_on_prototype` (the composite inspect lists
readable before writable only with prototype accessors; with own-
instance properties the order flips). The TS cell pins nothing
stream-semantic — its output is state-independent.

Migrated from api/streams/streams-test.js (whose KV-dependent
`partiallyReadStream` remains there).
