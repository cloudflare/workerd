# src/tests/streams/

Streams test suite, organized WPT-style: one subdirectory per functional area
(`identity/`, `encoding/`, `compression/`, `digest/`, `strategies/`,
`readable/`, `readable-byte/`, `writable/`, `transform/`, `piping/`,
`inspect/`, `r2-patterns/`, `iocontext/`). Every
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
throws(() => new FixedLengthStream(-1), expected);
```

`which-impl.js` reads
`globalThis.Cloudflare.compatibilityFlags['typescript_implemented_streams']`.
This pins every known divergence: if either implementation changes its side,
the corresponding cell fails. Document each divergence in the header comment
of the file that asserts it.

## Running

```
bazel test //src/tests/streams/... --nocache_test_results
```

Targets per suite: `<name>-cpp@`, `<name>-ts@`, each with `@all-compat-flags`
and `@all-autogates` variants, plus `@gc-stress` (off-by-default; run with
`--test_tag_filters=`). `workerd test` requires an exported `test()` handler
per case; assertions come from `node:assert` (`nodejs_compat` is in both
configs' flag lists).
