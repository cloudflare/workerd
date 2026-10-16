# src/tests/events/

Parity suites for `Event`, `CustomEvent` and `EventTarget`: every test runs
against both implementations, the C++ one (`src/workerd/api/basics.{h,c++}`)
and the TypeScript one (`src/per_isolate/events/`, behind
`typescript_implemented_events`). The suites follow
`src/tests/streams/AGENTS.md` (one behavior per file, explicit re-exports in
`main.js`, the module list defined once in `<suite>-modules.capnp`, flags
rather than dates, legacy cells, `which-impl` branches for every
divergence). This file records only what differs.

| Suite | Covers |
| --- | --- |
| `event/` | `Event` and `CustomEvent` outside dispatch |
| `event-target/` | listeners, options, dispatch, listener exceptions |

Each suite's `AGENTS.md` holds its divergence ledger, its flags table and its
module map. Rows marked † are gaps between the TypeScript classes and the
rest of the runtime (whose events and targets are still C++), pinned so
that integration flips them.

## Cells

| Cell | Flags |
| --- | --- |
| `<suite>-cpp` | `nodejs_compat` plus every date-gated flag the suite observes (each config lists them, one reason per flag) |
| `<suite>-cpp-pedantic` | the same plus `pedantic_wpt`, which changes `isTrusted` and `currentTarget` |
| `<suite>-ts` | `nodejs_compat`, `typescript_implemented_events`, `experimental`, the `per-isolate-javascript-bootstrap` autogate; `args = ["--experimental"]` |
| `<suite>-cpp-legacy` | `nodejs_compat` only, running `legacy-*.js` through `legacy-main.js`; no `@all-compat-flags` variant |

The TS cell pins none of the C++ flags: the TypeScript implementation
consults none of them, and its `@` and `@all-compat-flags` variants assert
that. (The one exception is indirect: the TS `{signal}` brand check reads
the C++ `AbortSignal`, whose layout follows
`workers_api_getters_setters_on_prototype`; event-target ledger #14.)

`which-impl.js` exports `usingTsImpl` (`typescript_implemented_events`) and
`pedanticWpt` (`pedantic_wpt`); the event-target suite's also exports
`gettersSettersOnPrototype`.

## Running

```
bazel test //src/tests/events/...
```

`@gc-stress` variants are `off-by-default`; run them by name with
`--test_tag_filters= --build_tag_filters=`.

## Events tests that live elsewhere

- `src/wpt/dom/events-test.ts` and `events-test-ts.ts`: WPT `dom/events`
  against the C++ and the TypeScript implementation.
- `src/workerd/api/tests/events-test.js`: the C++ implementation alone,
  under the modern flags: the runtime's Event subclasses
  (`ExtendableEvent`, `MessageEvent`, `ErrorEvent`, `CloseEvent`), the
  global as a target, and listener exceptions from a WebSocket. Its basic
  `Event` and `EventTarget` cases overlap these suites.
- `src/workerd/api/tests/legacy-dispatch-exceptions-test.js`: dispatch
  without `spec_compliant_dispatch_exceptions`.
- `src/workerd/api/tests/abortsignal-test.js`: `AbortSignal`, which stays C++
  until its own phase.
