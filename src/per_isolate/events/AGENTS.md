# src/per_isolate/events/

EventTarget plumbing for the bootstrap. Parent directory conventions
(primordials discipline, private-brand dispatch, no `instanceof`) apply; see
`src/per_isolate/AGENTS.md`.

## FILE MAP

| File | Role |
| ---- | ---- |
| `event-target-internals.ts` | The seam every bootstrap EventTarget subclass goes through to reach the EventTarget it extends: the base class, listener-count hooks, `on<type>` handler attributes, and dispatch with an explicit exception policy. The contract is in the file header. |

## KEY RULES

- A bootstrap EventTarget subclass extends `EventTarget` from
  `events/event-target-internals` and reaches EventTarget internals only
  through that module, never through `utils` or the `EventTarget` primordial
  directly. EventTarget itself is still C++; when it moves to the bootstrap,
  the module is reimplemented over it and subclasses are untouched.
- Listener-count hooks run synchronously inside `addEventListener()`,
  `removeEventListener()` and dispatch: they must neither run user code nor
  throw.
- Events the runtime fires are dispatched through the module's
  `dispatchEvent()`, with the policy from `effectiveExceptionPolicy()`, as
  C++ subclasses use `dispatchEventImpl()`. The public `dispatchEvent()` is
  for user-constructed events.
