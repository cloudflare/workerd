# src/tests/events/

Test suites for EventTarget subclasses implemented in the per-isolate
bootstrap (`src/per_isolate/`), one subdirectory per API
(`messagechannel/`, `eventsource/`). While an API's TypeScript
implementation is behind an autogate, its suite runs against **both**
implementations, the C++ one and the TypeScript one, to prove parity
(`eventsource/`). Once the gate and the C++ implementation are removed, the
suite runs against the TypeScript one alone (`messagechannel/`).

The suite rules are those of `src/tests/streams/AGENTS.md`, applied
verbatim: one file per behavior; `main.js` with explicit named re-exports;
the module list defined once in `<name>-modules.capnp` and shared by the
`<name>-cpp.wd-test` and `<name>-ts.wd-test` cells; compatibility flags
pinned by name, never a `compatibilityDate`; divergences branched on
`usingTsImpl` from `which-impl.js` and asserted exactly on each side; each
suite's `AGENTS.md` is the index of behaviors to tests plus the divergence
ledger. Read that file first.

## What is specific to this tree

While a suite covers both implementations:

- **The implementation is chosen by an autogate, not a compat flag.** The
  TypeScript cells name the API's autogate in the config's `autogates`
  list, and a `USING_TS_IMPL` JSON binding tells `which-impl.js` which
  implementation the cell is meant to run, since user code cannot read
  autogates. The C++ cells set `generate_all_autogates_variant = False`:
  enabling every autogate would swap in the TypeScript implementation
  while `which-impl.js` still reports C++. The TypeScript cells keep the
  variant, so "every autogate on" is still covered.
- **Legacy cells run on both implementations.** Unlike the streams, these
  TypeScript implementations honor the same compat flags the C++ ones do
  (they consult them through `compatFlags` in the bootstrap), so the
  unflagged behaviors are asserted for each, in `<name>-cpp-legacy` and
  `<name>-ts-legacy`.

Retiring the gate collapses the suite to one set of cells,
`<name>.wd-test`, `<name>-opt-in.wd-test` and `<name>-legacy.wd-test`, with
neither the autogate nor the binding. `which-impl.js` becomes `flags.js`,
each `usingTsImpl` branch keeps its TypeScript side, and the divergence
ledger folds into the suite's specification, since what it records is now
simply how the API behaves.

The seam the TypeScript subclasses share is
`src/per_isolate/events/event-target-internals.ts`; its C++ half is unit
tested in `src/workerd/api/event-target-bootstrap-test.c++`.

## Tests that live elsewhere (by design)

- `src/workerd/api/tests/messageport-postmessage-uaf-test.js`: a security
  regression single, deliberately not merged into the suite.
- `messagePortLegacyRedispatch` and `eventSourceLegacyError` in
  `src/workerd/api/tests/legacy-dispatch-exceptions-test.js`, which pins
  the legacy dispatch policy across several APIs at once.
- `src/workerd/api/eventsource-test.c++`: a unit test of the C++
  EventSource's native pump.

## Running

```
bazel test //src/tests/events/...
```
