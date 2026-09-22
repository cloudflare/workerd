# CountQueuingStrategy and ByteLengthQueuingStrategy

An informal specification of the two WHATWG queuing strategy classes as
implemented in workerd, derived from — and kept in lockstep with — the test
suite in this directory. **The tests are the normative artifact.** Both the
C++ implementation (`src/workerd/api/streams/readable.h`) and the
TypeScript implementation (`src/per_isolate/webstreams/strategies.ts`,
behind `typescript_implemented_streams`) are covered.

The WPT `streams/queuing-strategies.any.js` runs against both
implementations; its 12 C++ expectedFailures in `src/wpt/streams-test.ts`
correspond exactly to ledger entries #1–#6 below — the suite pins what the
C++ side actually does where WPT only records the failure.

## Divergence ledger (C++ vs TypeScript)

| # | Area | C++ | TypeScript | Pinned in |
| --- | --- | --- | --- | --- |
| 1 | size function identity | minted fresh on every property access | one shared function per class, stable | `sizeFunctionIdentity` |
| 2 | size function shape | name `""`, `prototype` present, constructable | name `"size"`, no prototype, not constructable (spec) | `sizeFunctionShape` |
| 3 | BLQS size `.length` | 0 | 1 (spec; CQS is 0 in both) | `sizeFunctionShape` |
| 4 | missing init dictionary message | jsg type-boundary TypeError ("not of type 'QueuingStrategyInit'.") | "init must be an object" | `initDictionaryIsRequired` |
| 5 | init bag without highWaterMark | accepted; `highWaterMark` is NaN | TypeError "init.highWaterMark is required" (spec: required member) | `missingHighWaterMarkDiverges` |
| 6 | BLQS size byteLength acquisition | internal slots for real BufferSources (shadowing getters ignored); `undefined` for null/undefined/non-objects | the spec's property read: throws on nullish, honors shadowing getters | `byteLengthSizeNullishDiverges`, `byteLengthSizeShadowingGetterDiverges` |
| 7 | fractional highWaterMark in stream accounting | truncated to an integer by the stream (`desiredSize` 0 for HWM 0.5) | fractional per spec | `fractionalAndZeroHighWaterMarks` |

Parity worth noting: `highWaterMark` is an unrestricted double stored
verbatim in both (NaN, ±Infinity, negatives, ToNumber coercions —
`highWaterMarkIsUnrestrictedDouble`); plain objects with a `byteLength`
property are counted by BOTH BLQS paths; detached buffers report 0;
strategies drive Readable/Writable desiredSize accounting identically for
integral HWMs; the class size functions work detached inside plain
strategy bags.

## Compatibility flags

The strategy classes consult no flags. The cpp cell pins
`streams_enable_constructors` (the integration module builds standard
streams), `workers_api_getters_setters_on_prototype`, and
`set_tostring_tag`; generic unflagged placement/branding behaviors are
guarded by the identity suite's legacy cell, so this suite carries no
legacy cell. `strategies-cpp-pedantic.wd-test` runs the full module set
with `pedantic_wpt` added, pinning the absence of pedantic effects.

## Assertion catalogue

| Module | Asserts |
| --- | --- |
| `api-surface.js` | branding; accessor placement + brand checks; no own instance props; hwm reflection incl. fractional; size identity (#1) and shape (#2, #3) |
| `construction.js` | init required with per-impl message (#4); missing highWaterMark (#5); unrestricted-double storage + ToNumber coercion |
| `size-semantics.js` | CQS constant 1; BLQS on views/buffers/DataViews with offsets, detached (0), length-tracking resizable views; plain-object byteLength reads; nullish handling (#6); shadowing getter (#6) |
| `integration.js` | strategies drive RS/WS desiredSize; detached class size fn in a plain bag; fractional HWM stream truncation (#7); zero HWM |
| `reentrancy.js` | plain-chunk byteLength getter runs inside the class size fn: throws propagate, re-entrant size calls work; size fns are receiver-agnostic; init-bag highWaterMark getter re-entering the constructor is safe (all parity). In-stream user-size-callback reentrancy is readable/writable-suite territory |
| `which-impl.js` | implementation detection |
