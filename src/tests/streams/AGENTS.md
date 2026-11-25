# src/tests/streams/

Streams test suite, organized WPT-style: one subdirectory per functional area
(`identity/`, `encoding/`, `compression/`, `digest/`, `strategies/`,
`readable/`, `readable-byte/`, `writable/`, `transform/`, `piping/`,
`inspect/`, `r2-patterns/`, `iocontext/`, `cache/`, `htmlrewriter/`,
`formdata/`, `sockets/`, `scaling/`). Every
test here runs against **both** streams implementations — the legacy C++ one
(`src/workerd/api/streams/`) and the TypeScript one
(`src/per_isolate/webstreams/`) — to prove parity. A test that only makes
sense for one implementation's internals belongs elsewhere.

## Structure of a suite directory

```
identity/
  identity-modules.capnp # THE module list, defined once (see below)
  identity-cpp.wd-test   # runs the modules against the C++ implementation
  identity-ts.wd-test    # runs the *same* modules against the TS implementation
  main.js                # entry point: explicit re-exports of every test
  which-impl.js          # exports usingTsImpl (see Divergences)
  <behavior>.js          # one file per behavior, small and single-purpose
  BUILD.bazel            # wd_test() targets over glob(["*.js"]) + the .capnp
```

- **One file, one behavior.** Decompose aggressively; the filename names the
  behavior. Shared setup helpers go in the file that needs them until a real
  need for a shared helper module appears.
- **`main.js` uses explicit named re-exports, never `export *`.** A name
  collision between two modules must be a load-time SyntaxError, not a
  silently dropped test.
- **The module list is defined exactly once**, as a
  `List(Workerd.Worker.Module)` constant in `<name>-modules.capnp`, which
  both main configs import and reference (`modules =
  IdentityModules.modules`). Sharing the test modules is the point:
  behavioral drift between the implementations fails one config or the
  other, and a single source of truth makes it impossible for the two cells
  to embed different code. A new test module is added in two places only:
  the `.capnp` constant and `main.js`. (`embed` paths resolve relative to
  the `.capnp` file; the legacy cell's deliberately different, smaller list
  stays inline in its own config.)

## Compatibility flags, not dates

Configs do **not** set `compatibilityDate`. The `wd_test` variant machinery
owns the date axis (`@` runs at 2000-01-01, `@all-compat-flags` at
2999-12-31) and the tests must pass at both extremes. Every date-gated
behavior a test depends on is pinned by naming its flag in **both** configs.
Each suite's `<name>-cpp.wd-test` documents its pinned set with a one-line
reason per flag, and the suite's AGENTS.md tables the flags against the
legacy tests that guard their unflagged sides. The TypeScript implementation
hard-codes the modern behaviors, so pinning them also keeps the two cells
comparable at the oldest date. Dateless opt-in flags that gate behaviors on
a suite's surface (e.g. `pedantic_wpt`) get a dedicated cell running the
full shared module set with the flag added, with the flag-off side pinned
in the legacy cells.

If a test fails only under `@all-compat-flags`, a date-gated flag changes the
behavior: identify it (`compatibility-date.capnp`) and pin it, don't pin a
date.

## Legacy (unflagged) cells

Pinning flags in the main configs means the suite never exercises the
pre-flag behaviors — the ones workers with old compat dates still depend on
and that backward compatibility protects. Each suite therefore also carries a
`<name>-cpp-legacy.wd-test` cell that omits the semantic flags entirely and
asserts the original behaviors as a regression guard (separate `legacy-*.js`
modules with their own `legacy-main.js` entry point). Legacy cells are
C++-only — the TypeScript implementation hard-codes the modern behaviors
regardless of these flags — and their `wd_test` sets
`generate_all_compat_flags_variant = False`, since the 2999-12-31 variant
would turn the omitted flags back on and contradict the cell's purpose.

## Main cells track current defaults

`<name>-cpp.wd-test` and `<name>-ts.wd-test` must always reflect the
behavior a newly created worker gets today. When a new streams-relevant
compatibility flag lands in `compatibility-date.capnp`, the suite is updated
in the same change:

1. **Pin the new flag in the main config of every implementation whose
   behavior it changes.** The C++ cell pins every semantic flag it is
   subject to. The TS cell stays minimal, pinning only flags the TypeScript
   implementation actually consults — its omissions are themselves an
   assertion of flag-indifference (see the comment in
   `identity-ts.wd-test`).
2. **Update the shared tests to assert the new default behavior**, using the
   `which-impl` pattern wherever the implementations diverge under the new
   flag.
3. **Preserve the outgoing behavior as a legacy test.** Add `legacy-*.js`
   module(s) asserting the pre-flag behavior, run by the unflagged
   `<name>-cpp-legacy.wd-test` cell. If the pre-flag behavior cannot be
   reproduced by the fully-unflagged cell — for example, it only manifests
   in combination with some other flag being on — add a dedicated legacy
   config pinning exactly that combination, again with
   `generate_all_compat_flags_variant = False`.

A main cell that lags the current defaults is a bug in the suite: it means
the behavior new workers actually receive is untested. Legacy assertions are
never deleted while the C++ implementation still serves workers pinned to
older compat dates.

## Divergences

Where the implementations deliberately differ, do not loosen the assertion to
whatever both happen to satisfy. Branch on the implementation and assert each
side's exact behavior:

```js
import { usingTsImpl } from 'which-impl';

const expected = usingTsImpl ? RangeError : TypeError;
throws(() => new IdentityTransformStream({ highWaterMark: -1 }), expected);
```

`which-impl.js` reads the cell's `USING_TS_IMPL` binding (see Selecting the
implementation). This pins every known divergence: if either implementation
changes its side, the corresponding cell fails. Document each divergence in
the header comment of the file that asserts it.

## Selecting the implementation

The `TS_WEB_STREAMS` autogate swaps in the TypeScript implementation for
the whole process, so the `<name>-ts.wd-test` config enables it in its
`autogates` list. User code cannot read autogates, so every cell of a suite
with a `which-impl.js` also declares the implementation it runs through a
`USING_TS_IMPL` JSON binding on its test worker: `true` in the TypeScript
cell, `false` in the C++ cells. The C++ cells set
`all_autogates_variant_excludes = ["ts-web-streams"]` in `BUILD.bazel`, so
that their `@all-autogates` variants enable every other autogate without
swapping in the TypeScript implementation.

A config that runs both implementations side by side, such as the
cross-implementation JS RPC tests, cannot use a process-wide autogate. It
keeps selecting the TypeScript implementation per worker with the
`typescript_implemented_streams` compatibility flag.

## What enabling TS_WEB_STREAMS changes

The autogate swaps the implementation for every Worker in the process,
whatever its compatibility date, so a Worker that runs on the C++
implementation meets every divergence the moment the gate is on; no
compatibility flag holds any of them back. Enabling it accepts all of
them. The suites' ledgers are the full list:

| Suite | Ledger rows |
| --- | --- |
| `compression/` | 17 |
| `digest/` | 3 |
| `encoding/` | 8 |
| `identity/` | 26 |
| `piping/` | 18 |
| `r2-patterns/` | 5 |
| `readable/` | 37 |
| `readable-byte/` | 46 |
| `strategies/` | 7 |
| `transform/` | 28 |
| `writable/` | 20 |

`inspect/` has no ledger: its one divergence is the whole suite, the
TypeScript streams having no custom `util.inspect`, so that every stream
inspects as a bare `ClassName {}`. The node suites (`src/tests/node/`)
carry ledgers of their own. Some kinds of divergence recur across suites:

- **Error messages.** Rows in nine of the ledgers differ only, or also, in
  a message, such as the TypeError a locked stream fails a pipe with
  (`piping/` #18).
- **Constructor source text**, which is `[native code]` only under C++
  (`encoding/` #5, and `identity/`'s `api-surface.js`).

## Running

```
bazel test //src/tests/streams/...
```

Targets per suite: `<name>-cpp@`, `<name>-ts@`, each with an
`@all-compat-flags` variant, plus `@gc-stress` (off-by-default; run with
`--test_tag_filters=`), and an `@all-autogates` variant, which leaves the
C++ cells on the C++ implementation.
`workerd test` requires an exported `test()` handler per case; assertions
come from `node:assert` (`nodejs_compat` is in both configs' flag lists).
The `scaling/` suite is the exception to the variant set: it times shapes at
80k-160k entries, so it has no `@gc-stress` variant, skips the sanitizer
builds, and its C++ cell is `off-by-default` (see its `AGENTS.md`).

## Streams tests that live elsewhere (by design)

- `src/tests/node/` — the Node.js compatibility layer's USE of web
  streams (`node:stream` adapters, `node:net` sockets, the `node:http`
  client and server), under the same suite rules and the same cpp/ts
  cells; each suite's `AGENTS.md` there carries its own ledger. A pure
  web-streams behavior found through the node layer is pinned here, not
  there (e.g. `readable` ledger #18).
- `src/workerd/api/tests/js-rpc-streams-*` — stream serialization over
  JS RPC (including ts-impl cells); owned by the worker-rpc domain.
- `src/workerd/api/tests/ts-webstreams-test.js` — TypeScript-impl
  internals (native/buffer/iterable bodies ARE ts streams, pumpTo);
  single-implementation by nature.
- Security regression singles in `src/workerd/api/tests/`
  (streams-byob-close-reentry, streams-byob-concurrent-readatleast,
  streams-byte-cancel-uaf, streams-byte-handlePush-uaf,
  streams-circ-ref-regression, streams-consumer-reentry-gc,
  streams-internal-read-buffer-gc, autovuln-*) — authoritative
  crash/UAF repros, deliberately not merged into suites.
- `src/workerd/api/streams/streams-test.js` — `partiallyReadStream`
  (needs a KV binding).

## IDL shape (deliberately not pinned here)

WebIDL function metadata — operation `.length` values (optional
arguments do not count), and promise-typed attributes/operations
REJECTING rather than throwing on a broken `this` — is enumerated
per-implementation by WPT's `idlharness.any.js`: the C++ implementation
carries the known deviations as expectedFailures in
`src/wpt/streams-test.ts`; the TypeScript implementation matches spec.
The suites do not duplicate that enumeration.
