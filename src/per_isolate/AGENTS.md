# src/per_isolate/

Per-isolate JavaScript/TypeScript bootstrap: scripts that run synchronously
at context creation, before any user code. Gated by the
`per-isolate-javascript-bootstrap` autogate (config:
`workerd-autogate-per-isolate-javascript-bootstrap`). C++ entry point:
`src/workerd/io/per-isolate-bootstrap.c++`.

## EXECUTION MODEL

- Scripts are compiled as functions with a context-extension object — the
  pseudo-globals `require`, `module`, `exports`, `compatFlags`,
  `autogates`, `primordials`, `utils` are in scope but NOT on
  `globalThis` (see `per_isolate-env.d.ts`).
- Module system is bootstrap CommonJS: `require('webstreams/queue')` +
  `module.exports = {...}`. The `src/node/` ESM-only rule does NOT apply
  here. Circular requires are a FATAL startup error — keep modules
  acyclic (e.g., `webstreams/native` is a deliberate leaf).
- TypeScript `import type` / `export type` are used freely for type
  plumbing (fully erased; the loader only sees `module.exports`).
- `main.ts` installs the (TEMPORARY, dev-only) lazy `globalThis.streams`
  surface.
- Files are auto-discovered (`BUILD.bazel` and `tsconfig.json` both glob
  `**/*.ts`). No registration needed for new files.
- Local convention: no copyright headers in this directory's bootstrap
  scripts (deviation from the repo-wide new-file rule; keep consistent).

## CONVENTIONS

- **Primordials discipline**: no bare prototype lookups on builtins after
  bootstrap captures; no `for...of` over patchable iterables. Capture
  methods/getters at module scope via `uncurryThis`.
- **JSG property capture trap**: readonly JSG properties live on the
  PROTOTYPE only under modern compat
  (`workers_api_getters_setters_on_prototype`, default-on 2022-01-31).
  Under older dates JSG uses `SetNativeDataProperty` on the INSTANCE
  template — an own native DATA property per instance; **no getter
  function exists anywhere to capture**. Use `captureJsgGetter` in
  the `primordials.ts` : prototype-accessor capture with a plain-read fallback
  (an own data property read never consults the patchable prototype chain).
  The bootstrap runs for every worker regardless of compat date — both layouts
  must work. JSG *methods* are always on the prototype.
- Internal cross-module surfaces are exported as a single namespace
  object (`internalsForPipe`, `nativeStreamInternals`), never
  re-exported to users.
- Internal class dispatch uses private-brand `in` checks, NEVER
  `instanceof` (classes reachable from the global have user-reachable
  `Symbol.hasInstance`).

## WEBSTREAMS ARCHITECTURE (webstreams/)

TypeScript Streams implementation with a backend-blind reader layer and
two consumer backends behind the `StreamConsumer`/`ByteStreamConsumer`
fence (authoritative docs are IN-SOURCE — read these headers first):

| File          | Role                                                                                         |
| ------------- | -------------------------------------------------------------------------------------------- |
| `queue.ts`    | QUEUED backend: single-queue/multi-cursor, JS sources; fence interfaces; invariant list      |
| `native.ts`   | NATIVE backend: C++-(eventually-)backed pull conduit; **the C++/JS contract** + invariants   |
| `readable.ts` | Reader layer + queued controllers + the BACKEND-DISPATCH points (constructor, tee, chains, byte-capable gate, JS-to-C++ extraction) |
| `writable.ts` / `transform.ts` / `strategies.ts` | WHATWG writable/transform/strategies                                     |
| `identity.ts` | IdentityTransformStream and FixedLengthStream (byte-capable identity transforms)             |
| `encoding.ts` | TextEncoderStream and TextDecoderStream (pure JS codec transforms)                           |
| `streams.ts`  | Module aggregator and temporary native-source exports                                        |
| `types.d.ts`  | TypeScript type definitions for the streams API                                              |

Key rules:

- The reader layer must stay backend-blind; backend divergence is
  confined to the fence interface and the marked BACKEND-DISPATCH points.
- Do not port logic across the fence without checking BOTH invariant
  lists (`queue.ts` and `native.ts` headers).
- The native source contract (marker symbol, standard pull/cancel hooks,
  byobRequest discrimination, once-per-pull delivery, per-pull abort
  signal for cancellation, under-delivery = fused
  `{done: true, value: partial}` EOF, tee hook, `expectedLength`
  exact-total byte contract) is specified in the `native.ts` header.
  The future C++ integration MUST conform to it; it is currently exercised
  by JS mocks only. Key addition: `pull` receives an extension `signal`
  argument — the source checks `signal.aborted` before delivery and stashes
  bytes for redelivery if aborted (race buffering lives source-side; the JS
  conduit is uniformly bufferless).
- `kNativeSource` is TEMPORARILY re-exported via `streams.ts` for tests;
  it is removed when the real C++ handshake lands.

## ANTI-PATTERNS

- **NEVER** assume a JSG readonly property has a capturable getter (see
  the capture trap above).
- **NEVER** add a runtime require cycle between bootstrap scripts.
- **NEVER** expose internals on user-visible exports (the temporary
  `kNativeSource` exception is tracked for removal).
- **NEVER** rely on `readable-source-adapter.h` as a reference for the
  native streams contract — it belongs to the original (non-enabled)
  implementation; `native.ts` is authoritative.
