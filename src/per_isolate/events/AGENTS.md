# src/per_isolate/events/

TypeScript `Event`, `CustomEvent` and `EventTarget` (DOM §2), installed on
the global by `main.ts` under `typescript_implemented_events`. The C++
classes (`src/workerd/api/basics.{h,c++}`) stay registered: the runtime's
own events (`MessageEvent` and the rest) and targets (the global,
`AbortSignal`) are still C++, and the two implementations accept none of
each other's objects. File headers are the authoritative docs.

Parent directory conventions (primordials discipline, the JSG capture trap,
private-brand dispatch, no `instanceof`) apply here; see
`src/per_isolate/AGENTS.md`.

## FILE MAP

| File | Role |
| --- | --- |
| `common.ts` | WebIDL helpers (`toDictionary`, `notEnoughArguments`, `toStringTagProperty`, ...) and `requireAbortSignal`; leaf module |
| `event.ts` | `Event`, `CustomEvent`, and `eventInternals`: the frozen, null-prototype namespace of the spec steps `event-target.ts` performs on an event (`beginDispatch`, `enterInvoke`, `endDispatch`, ...); requires `common` |
| `event-target.ts` | `EventTarget`: listener storage, the options' conversion, dispatch, the `{signal}` abort algorithm; requires `common` and `event` |
| `events.ts` | Aggregator of the user-visible classes |

## INVARIANTS

- **The TS classes consult no C++ compat flag.** They hard-code the modern,
  spec behavior at every compat date. The parity suites
  (`src/tests/events/`) pin every place this differs from the C++ classes,
  in their `AGENTS.md` ledgers; a behavior change here updates the ledger
  row and its test.
- **Event state is private fields**, one `#flags` word for the spec's flags.
  `event-target.ts` reaches it only through `eventInternals`, never the
  public attributes, which user code can shadow.
- **`isTrusted` is an own, non-configurable accessor** defined in the
  constructor ([LegacyUnforgeable]), with one getter shared by every
  instance so their maps stay shared.
- **Listener records are null-prototype**; a type's list is created by the
  first add and deleted once it empties. Removal marks the record
  `removed` before splicing it, which is what a dispatch's clone of the
  list checks.
- **Listeners are called through the captured `FunctionPrototypeCall`**,
  and an object listener's `handleEvent` is read at every call. Results are
  ignored.
- **Listener exceptions go to the global `reportError()` captured at load**
  (not the property's current value), and never reach the
  `dispatchEvent()` caller. A `reportError()` that itself throws (a
  throwing `error` listener without `spec_compliant_dispatch_exceptions`)
  is dropped.
- **The `{signal}` abort algorithm holds its record through a `WeakRef`**,
  so a long-lived signal never retains a dead target or its listeners; the
  record holds the registration handle, whose collection unregisters the
  algorithm.
- **`requireAbortSignal` brand-checks through the C++ `aborted` getter.**
  Under the instance-property layout (old compat dates) there is no getter,
  and the check accepts any object whose `aborted` is a boolean
  (event-target ledger #14).

## ANTI-PATTERNS

- **NEVER** read an event's public attributes, or call its public methods,
  from `event-target.ts`; go through `eventInternals`.
- **NEVER** capture `globalThis.EventTarget` or `globalThis.Event` in
  another bootstrap module: under the flag they are these classes, and
  without it the C++ ones. `require('events/events')` for these.
