# src/tests/structured-clone/

Tests for the global `structuredClone()`. Every test runs against both
implementations:

- the C++ JSG method, `ServiceWorkerGlobalScope::structuredClone` in
  `src/workerd/api/global-scope.c++`;
- the TypeScript one, `src/per_isolate/structured-clone.ts`. `main.ts` installs
  it under the experimental `typescript_implemented_structured_clone` flag,
  which needs the `per-isolate-javascript-bootstrap` autogate.

The layout follows `src/tests/streams/AGENTS.md`:

- `structured-clone-modules.capnp` lists the modules once.
- `structured-clone-cpp.wd-test` and `structured-clone-ts.wd-test` run that list.
- `main.js` re-exports every test by name.
- `which-impl.js` exports `usingTsImpl`.

Both implementations clone with the same native serializer, `jsg::structuredClone`
in `src/workerd/jsg/ser.c++`. The TypeScript one reaches it through
`utils.structuredClone()`. Everything about the clone itself is therefore parity:
which values clone, which objects transfer, and the `DataCloneError` and
`TypeError` texts (`cloning.js`). The implementations differ only in the
WebIDL layer in front of the serializer. TypeScript follows WebIDL there; C++
follows JSG's conversions.

## Divergence ledger (C++ vs TypeScript)

| # | Area | C++ | TypeScript | Pinned in |
| --- | --- | --- | --- | --- |
| 1 | `structuredClone.length` | 0 | 1 (WebIDL: the optional options argument does not count) | `functionShape` |
| 2 | Source text | `function structuredClone() { [native code] }` | the method's source | `sourceText` |
| 3 | Values accepted as `transfer` | Arrays only, so not Sets, iterators, or Proxies of arrays | any iterable (WebIDL `sequence<object>`), including those three | `iterablesOtherThanArrays` |
| 4 | Message when `transfer` is rejected (primitives, `null`, non-iterable objects; a TypeError in both) | `... is not of type 'Array'.` | `... is not of type 'sequence<object>'.` | `nonObjectMemberRejected`, `nonIterableObjectRejected` |
| 5 | A non-object `transfer` element, including an array hole (a TypeError in both) | reaches the serializer: `Object is not transferable` | rejected by the conversion: `... an element of the 'transfer' field ... not of type 'object'.` | `nonObjectElementsRejected` |
| 6 | An array's `[Symbol.iterator]` | not called; elements read by index | called once (WebIDL iterates the sequence, user iterator by contract) | `arrayIteratorUse` |

## Parity worth knowing

All of these are spec deviations shared by both implementations, and so pinned
as parity:

- **Receiver.** `undefined`, `null` and the global are accepted. Any other
  receiver throws JSG's illegal-invocation `TypeError`. The check runs before
  any argument is read.
- **Argument count.** A call with no arguments throws JSG's `parameter 1 is
  not of type 'Value'` `TypeError`.
- **Options.** `undefined` and `null` mean no options. Any object, including
  functions and arrays, is a dictionary. A primitive throws `parameter 2 is
  not of type 'StructuredCloneOptions'`.
- **When `transfer` is read.** It is read once, through the prototype chain,
  before the value is serialized.
- **Transfer semantics.** These come from the serializer:
  - A view transfers its buffer.
  - Duplicates and already-detached buffers are accepted, where the spec says
    DataCloneError.
  - A non-ArrayBuffer object throws a `TypeError`, where the spec says
    DataCloneError.
  - Shared memory is not transferable.
  - Buffers are detached only after the value is written.

## Running

```
bazel test //src/tests/structured-clone/...
```

Targets: `structured-clone-cpp@` and `structured-clone-ts@`, each with
`@all-compat-flags` and `@all-autogates` variants. `@all-autogates` on the C++
cell turns on the bootstrap without the flag, so it still runs C++.
