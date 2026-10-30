# EventTarget

`EventTarget`: adding and removing listeners, the options, dispatch, how a
listener is called, the `{signal}` option, listener exceptions, and the
WebIDL shape. **The tests are the normative artifact.** An event's own
state outside dispatch is the event suite's.

The C++ column is `event-target-cpp` (the date-gated flags on,
`pedantic_wpt` off); where `event-target-cpp-pedantic` differs, it is noted.
A target has no parent, so a dispatch's path is the target alone, and both
of the spec's passes (capture listeners, then the rest) run at it.

## Divergence ledger (C++ vs TypeScript)

| # | Area | C++ | TypeScript | Pinned in |
| --- | --- | --- | --- | --- |
| 1 | `currentTarget` once dispatch ends | the target (`pedantic_wpt`: `null`) | `null` | `attributesAfterDispatch` |
| 2 | `dispatchEvent()` after `preventDefault()` on an event that is not cancelable | `false`, though `defaultPrevented` stays `false` | `true` (spec) | `dispatchResult`, `listenerReturningTrue` |
| 3 | an event canceled before dispatch, or in an earlier one | the flag is unset as dispatch starts: listeners see `defaultPrevented` `false`, the result is `true` | stays canceled: only `initEvent()` unsets the flag (spec) | `canceledFlagSurvivesDispatch` |
| 4 | the stop propagation flags across dispatches | stop propagation is never consulted, and neither flag is unset: `stopPropagation()` before dispatch stops nothing and `cancelBubble` stays `true`; an event stopped immediately never reaches a listener again | a stopped event reaches no listener, and both flags are unset as dispatch ends (spec) | `stopFlagsSetBeforeDispatch`, `stopFlagsSetDuringDispatch` |
| 5 | capture (`useCapture`, `{capture}`) | TypeError | part of a listener's identity; capture listeners run first, in a pass of their own (spec) | `captureOption`, `captureListenersRunFirst`, `captureIsPartOfIdentity`, `stopPropagationInCaptureSkipsBubbling`, `listenerAddedDuringCaptureRunsInBubbling` |
| 6 | `{passive: true}` | TypeError | `preventDefault()` does nothing in the listener; not part of identity (spec) | `passiveListenerCannotCancel`, `passiveIsNotPartOfIdentity`, `passiveFlagResetsAfterAThrow` |
| 7 | the options' read order | `capture`, `passive`, `once`, `signal` | `capture`, `once`, `passive`, `signal` (WebIDL: the inherited member, then the rest in lexicographic order) | `optionsReadOrder` |
| 8 | `removeEventListener()` with capture | TypeError, even for a null listener | removes the matching capture listener, if any | `removeWithCapture` |
| 9 | an object listener without a callable `handleEvent` (`{}`, `[]`) | TypeError from `addEventListener()` | accepted (a WebIDL callback interface); each call reports a TypeError, and a `handleEvent` defined later is used | `nonCallableObjectListener`, `nonCallableHandleEventIsReported` |
| 10 | when `handleEvent` is looked up | once, as the listener is added: a throwing getter throws from `addEventListener()`, and a replacement is never seen | at every call: a throwing getter's exception is reported like a listener's (WebIDL) | `handleEventLookup`, `handleEventReplacedAfterAdd`, `throwingHandleEventGetter` |
| 11 | a listener's result | `true` calls `preventDefault()`; any other non-boolean result logs a warning, once per target (console only, so unpinned) | ignored (spec) | `listenerReturningTrue` |
| 12 | a callable `on<type>` property (own, or a subclass's method) | a listener for `<type>`, called before the target's listeners with the global as `this` | not consulted: `EventTarget` declares no event handler attributes (spec) | `onTypeProperty`, `onTypeMethod` |
| 13 | a listener removed and re-added during the dispatch | runs in that dispatch | does not: the dispatch runs over a clone of the list, and the re-added listener is a new registration (spec) | `reAddedListenerDuringDispatch` |
| 14 | a forged `{signal: {aborted: <boolean>}}` | TypeError | without `workers_api_getters_setters_on_prototype` the C++ `AbortSignal` has no `aborted` getter to brand-check with, so the forgery passes: `utils.addAbortAlgorithm()` rejects it once a listener is to be added, but a null listener, or a forgery claiming to have aborted, adds nothing silently; TypeError with the layout | `forgedSignal` |
| 15 | method `length` | `0` | `2`, `2`, `1` (WebIDL) | `functionMetadata` |
| 16 | `Symbol.toStringTag` | writable | non-writable (WebIDL) | `toStringTagBranding` |
| 17 | the interface object's `prototype` property | writable | non-writable (WebIDL) | `interfaceObject` |
| 18 | `cloudflare:internal-class` marker | on the prototype | absent | `internalClassMarker` |
| 19 | a listener exception outside any request (module evaluation) | logged only | reported as within one: the global `error` event fires, then the exception is logged unless a listener cancels it (HTML's "report an exception") | `exceptionsOutsideARequest` |
| 20† | events and targets of the other implementation | (one implementation) | a C++ event (the runtime's, such as `MessageEvent`) cannot be dispatched at a TS target, nor a TS event at a C++ one (the global, `AbortSignal`): TypeError. Neither implementation's methods accept the other's targets | `eventsAcrossImplementations`, `methodsAcrossImplementations`, `theGlobal` |
| 21† | `AbortSignal instanceof EventTarget` | `true` | `false`: `AbortSignal` extends the C++ class | `abortSignalIsNotAnEventTarget` |
| 22† | `node:events`' `setMaxListeners()` and `getEventListeners()` given an `AbortSignal` | accepted | `ERR_INVALID_ARG_TYPE`: they check `instanceof EventTarget` | `nodeEventsHelpers` |
| 23† | `structuredClone(target)` | DataCloneError | an ordinary object of the target's own enumerable properties | `structuredCloneDiverges` |

Parity worth noting (pinned): argument counts and brand checks (the
TypeError types match; messages differ), including `addEventListener(type)`
without a listener; detached methods throwing, while the global's own
methods work called bare; arguments converted in order, before any step
runs, for a null listener too; duplicates ignored, removal by `(type,
callback, capture)` and registration order; a function listener's `this`
the target and an object listener's the object; `once` removing the
listener before calling it; a listener added during a dispatch waiting for
the next one, and one removed during it not running; `InvalidStateError` for
an event already being dispatched; nested dispatch; `{signal}` removing the
listener through an abort algorithm (before the signal's `abort` listeners,
never for a synthetic `abort` event, and only the registration it was added
with: a duplicate's signal is ignored, and an abort after remove and re-add
leaves the new registration); listener exceptions reported in order through
the runtime's own `reportError()` (not the global property), synchronously,
with the dispatch carrying on; a polluted `Object.prototype` reaching only
the user's own options and listener objects, and patched built-ins never
used; a long-lived signal retaining neither dead targets nor their
listeners.

## Compatibility flags

| Flag | Pinned in main cells | Unflagged side |
| --- | --- | --- |
| `workers_api_getters_setters_on_prototype` (2022-01-31) | C++ cells | `event-target-cpp-legacy`: `srcElement` reads the current target, so it is `null` once dispatch ends (`legacy-dispatch-attributes.js`). The TS cell runs at both layouts of the C++ `AbortSignal` (row 14) |
| `dont_substitute_null_on_type_error` (2022-06-01) | C++ cells | `event-target-cpp-legacy`: a primitive listener is taken as null (`legacy-listener-conversion.js`) |
| `set_tostring_tag` (2024-09-26) | C++ cells | `event-target-cpp-legacy`: no `Symbol.toStringTag` (`legacy-to-string-tag.js`) |
| `set_event_target_this` (2025-08-01) | C++ cells | `event-target-cpp-legacy`: a function listener's `this` is the global (`legacy-listener-this.js`) |
| `spec_compliant_dispatch_exceptions` (2026-09-15) | C++ cells | the first listener exception propagates: `src/workerd/api/tests/legacy-dispatch-exceptions-test.js` |
| `enable_weak_ref` (2025-05-05) | every cell, for `gc.js`'s `FinalizationRegistry` | none on this surface |
| `pedantic_wpt` (dateless) | `event-target-cpp-pedantic` | the main C++ cell (row 1) |

## Module map

| Module | Coverage |
| --- | --- |
| `api-surface.js` | `Symbol.toStringTag`, prototype methods, function metadata, the interface object, brand checks, detached methods; rows 15-18 |
| `listeners.js` | argument counts and conversion, nullish and primitive listeners, duplicates, removal, order, invocation, per-target registrations; row 9 |
| `options.js` | inert options, capture, passive, read order, `once`, removal with capture; rows 5-8 |
| `dispatch.js` | `dispatchEvent()`'s argument and result, attributes during and after dispatch, the canceled and stop flags, the list changing during dispatch, reentrant and nested dispatch, odd types; rows 1-4, 13 |
| `handle-event.js` | `handleEvent` lookup, listener results; rows 9-11 |
| `signal.js` | the `{signal}` option; row 14 |
| `exceptions.js` | listener exceptions, inside and outside a request; row 19 |
| `reflection.js` | `on<type>` properties; row 12 |
| `interop.js` | the global, `AbortSignal`, `node:events`, `structuredClone()`, `util.inspect()`; rows 20-23 |
| `subclassing.js` | subclasses of `EventTarget` (parity) |
| `pollution.js` | polluted `Object.prototype` and patched built-ins (parity) |
| `gc.js` | what keeps listeners and targets alive (parity) |
| `helpers.js` | `collectReportedErrors()`, and the C++ `Event` and `EventTarget` for the cross-implementation tests |
| `legacy-*.js` | the legacy cell (see the flags table) |
