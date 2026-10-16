# Event and CustomEvent

`Event` and `CustomEvent` outside dispatch: construction, attributes,
cancelation, the propagation flags, `initEvent()`, `detail`, and their
WebIDL shape. **The tests are the normative artifact.** How dispatch reads
and resets an event's state is the event-target suite's.

The C++ column is `event-cpp` (the date-gated flags on, `pedantic_wpt` off);
where `event-cpp-pedantic` differs, it is noted.

## Divergence ledger (C++ vs TypeScript)

| # | Area | C++ | TypeScript | Pinned in |
| --- | --- | --- | --- | --- |
| 1 | `isTrusted` placement | a prototype getter (`pedantic_wpt`: an own, enumerable, non-writable, configurable data property); either can be shadowed, by a subclass field for one | an own, enumerable, non-configurable accessor on every instance, with one shared getter ([LegacyUnforgeable]) | `isTrustedPlacement`, `isTrustedCannotBeRedefined` |
| 2 | `target`, `srcElement` before any dispatch | `undefined` | `null` | `defaultAttributes` |
| 3 | `currentTarget` before any dispatch | `undefined` (`pedantic_wpt`: `null`) | `null` | `defaultAttributes` |
| 4 | `timeStamp` | always `0` | `Date.now()` as the event is constructed; `0` outside a request | `timeStampIsDateNowAtConstruction` |
| 5 | `returnValue` setter | none: assignment throws in strict code | `false` cancels, as `preventDefault()` does | `returnValueSetterCancels` |
| 6 | `cancelBubble = false` | unsets the stop propagation flag | does nothing (spec) | `cancelBubbleFalseIsNoOp` |
| 7 | `cancelBubble` after `stopImmediatePropagation()` | `false` | `true`: it sets the stop propagation flag too (spec) | `stopImmediatePropagationSetsCancelBubble` |
| 8 | a setter called without an argument | `cancelBubble`'s takes it as `false` | TypeError (WebIDL) | `setterArity` |
| 9 | `initEvent()`, `initCustomEvent()` | absent | present, re-running initialization (ported from WPT's `Event-initEvent.html`) | `init-event.js` (6 tests) |
| 10 | `detail` when the dictionary has none | `undefined` | `null` (spec) | `detailDefaultDiverges` |
| 11 | function `length`, accessor `name` | `0`, `""` | the WebIDL values | `functionMetadata` |
| 12 | `NONE`, `CAPTURING_PHASE`, `AT_TARGET`, `BUBBLING_PHASE` | configurable | non-configurable (WebIDL) | `phaseConstants` |
| 13 | `Symbol.toStringTag` | writable | non-writable (WebIDL) | `toStringTagBranding` |
| 14 | `CustomEvent`'s interface object | does not inherit from `Event`, so it has none of `Event`'s statics | inherits from `Event` (WebIDL) | `interfaceObjects` |
| 15 | `cloudflare:internal-class` marker | on the prototypes | absent | `internalClassMarker` |
| 16 | the interface objects' `prototype` property | writable | non-writable (WebIDL) | `interfaceObjects` |
| 17† | `structuredClone(event)` | DataCloneError | an ordinary object holding the own `isTrusted` | `structuredCloneDiverges` |
| 18† | `util.inspect(event)` | every attribute | the own `isTrusted` only | `inspectDiverges` |
| 19† | the runtime's events (`MessageEvent`, `ErrorEvent`, `CloseEvent`, ...) | instances of the global `Event` | C++ events, not instances of the global `Event` | `runtimeEventsAreCppEvents` |

Rows 11 and 12 are the C++ side without the experimental
`spec_compliant_property_attributes`, and row 16 without the dateless
`immutable_api_prototypes`; WPT's cells turn both on.

Parity worth noting (pinned): argument counts and brand checks (the
TypeError types match; messages differ); the `type` and dictionary
conversions, including each member read once, in WebIDL order, with
ToBoolean; `preventDefault()` needing `cancelable`; `stopPropagation()`
setting `cancelBubble`; `detail` returned as given; subclasses (attribute
overrides, `Reflect.construct()`, `new.target`); an omitted dictionary
reading nothing from a polluted `Object.prototype`, and construction
untouched by patched built-ins.

## Compatibility flags

| Flag | Pinned in main cells | Unflagged side |
| --- | --- | --- |
| `workers_api_getters_setters_on_prototype` (2022-01-31) | C++ cells | `event-cpp-legacy`: the attributes are per-instance data properties (enumerable and configurable; only `cancelBubble` writable, through its setter), a subclass cannot override one, and `srcElement` reads the current target (`legacy-instance-properties.js`) |
| `set_tostring_tag` (2024-09-26) | C++ cells | `event-cpp-legacy`: no `Symbol.toStringTag` (`legacy-to-string-tag.js`) |
| `pedantic_wpt` (dateless) | `event-cpp-pedantic` | the main C++ cell (rows 1 and 3) |

## Module map

| Module | Coverage |
| --- | --- |
| `api-surface.js` | members' placement and attributes, `Symbol.toStringTag`, constants, interface objects, function metadata, brand checks, `new` required; rows 1, 11-16 |
| `construction.js` | argument conversion, default attributes, `composedPath()`, `timeStamp`; rows 2-4 |
| `cancelation.js` | `preventDefault()`, `defaultPrevented`, `returnValue`; row 5 |
| `propagation-flags.js` | `stopPropagation()`, `stopImmediatePropagation()`, `cancelBubble`; rows 6-8 |
| `init-event.js` | `initEvent()`, `initCustomEvent()`; row 9 |
| `custom-event.js` | `detail`; row 10 |
| `subclassing.js` | subclasses of both classes, dispatching them; row 1 |
| `pollution.js` | polluted `Object.prototype` and patched built-ins (parity) |
| `interop.js` | `structuredClone()`, `util.inspect()`, the runtime's events; rows 17-19 |
| `legacy-instance-properties.js`, `legacy-to-string-tag.js` | the legacy cell (see the flags table) |
