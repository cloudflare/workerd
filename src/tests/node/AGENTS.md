# src/tests/node/

Test suites for the parts of the Node.js compatibility layer (`src/node/`)
that sit on top of the Web Streams implementation: the `node:stream`
web-interop adapters and `node:net` sockets over `connect()` stream
halves. One subdirectory per area. Every test
here runs against **both** streams implementations — the legacy C++ one
(`src/workerd/api/streams/`) and the TypeScript one
(`src/per_isolate/webstreams/`) — to prove the node layer behaves
identically on either.

The suite rules are those of `src/tests/streams/AGENTS.md`, applied
verbatim: one file per behavior; `main.js` with explicit named re-exports;
the module list defined once in `<name>-modules.capnp` and shared by the
`<name>-cpp.wd-test` and `<name>-ts.wd-test` cells; compatibility flags
pinned by name, never a `compatibilityDate`; divergences branched on
`usingTsImpl` from `which-impl.js` and asserted exactly on each side;
legacy C++-only cells (`generate_all_compat_flags_variant = False`) for
pre-flag behaviors; each suite's `AGENTS.md` is the index of behaviors to
tests plus the divergence ledger. Read that file first.

## What is specific to this tree

- The subject is the **node layer's use** of streams, not the streams
  themselves. A test belongs here when it observes how `src/node/` code
  drives a `ReadableStream`/`WritableStream` (locks, backpressure,
  close/abort/cancel propagation, chunk types) or how a web stream surfaces
  through a node API. Pure web-streams behavior belongs in
  `src/tests/streams/`; pure node-API behavior with no stream underneath
  (header parsing, `BlockList`, option validation) stays in
  `src/workerd/api/node/tests/`.
- Under the C++ implementation the adapters construct streams with
  `new ReadableStream()`/`new WritableStream()`, which require
  `streams_enable_constructors`; the C++ cells pin it and the legacy cells
  pin the constructor-gate `Error` that older compat dates still produce.
  The TypeScript implementation does not consult the flag.
- `nodejs_compat` is date-enabled (2026-08-04) and is pinned explicitly so
  the `@` variant (2000-01-01) still loads `node:*`.
- Suites that talk to a peer use a node sidecar (`js_binary` +
  `sidecar_port_bindings`), following `src/tests/streams/sockets/`.

## Suites

| Directory | Subject | Peer |
| --- | --- | --- |
| `stream/` | `Readable/Writable/Duplex.toWeb/fromWeb`, `pipeline`, `stream/web`, `stream/consumers` | none |
| `net/` | `net.Socket` over the `connect()` socket's BYOB reader and writer | node sidecar TCP servers |

## Running

```
bazel test //src/tests/node/... --nocache_test_results
```
