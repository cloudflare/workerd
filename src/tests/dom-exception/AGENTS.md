# DOMException

An informal specification of DOMException as implemented in workerd, kept
in lockstep with the test suite in this directory. **The tests are the
normative artifact.** The cells run the same modules
(`dom-exception-modules.capnp`):

| Cell | Implementation | Flags the suite branches on |
| --- | --- | --- |
| `dom-exception-cpp` | `jsg/dom-exception.{h,c++}` | `set_tostring_tag`, `enhanced_error_serialization` |
| `dom-exception-legacy-flags-cpp` | the same | `do_not_set_tostring_tag`, `legacy_error_serialization` |
| `dom-exception-opt-in-cpp` | the same, with the dateless WebIDL opt-ins | as `dom-exception-cpp`, plus `spec_compliant_property_attributes`, `immutable_api_prototypes` |
| `dom-exception-ts`, `dom-exception-legacy-flags-ts`, `dom-exception-opt-in-ts` | `src/per_isolate/dom-exception.ts`, under the `TS_DOM_EXCEPTION` autogate | as the C++ cell of the same name |

`which-impl.js` reads the flags; a cell's `USING_TS_IMPL` binding says which
implementation it means to run. The C++ cells have no `@all-autogates`
variant, since enabling every autogate enables `TS_DOM_EXCEPTION`. Every
cell needs `--experimental`, for
`service_binding_extra_handlers` (crafted payloads through
`Fetcher.queue()`), `enable_web_file_system` and `enable_abortsignal_rpc`.

What JavaScript cannot see is pinned in C++: the bytes a DOMException
serializes to, what reading tags 7 and 8 creates, the `kj::Exception` it
tunnels as and `reportError()`'s description, in
`api/dom-exception-bootstrap-test.c++`, which runs with the per-isolate
bootstrap; its `@all-autogates` variant runs them against the TypeScript
implementation.

## Core semantics

- **Construction:** `new DOMException(message = '', name = 'Error')`; both
  arguments are converted to strings, the message first, and the second is
  a name, never an options bag. `code` is the legacy code of the name, or 0.
  Calling without `new` throws `TypeError`.
- **Errorness:** `instanceof Error`; the prototype inherits from
  `Error.prototype`; `String(e)` is `Name: message` (`Name` alone for an empty
  message). Attributes are read-only, but own properties defined on an
  instance shadow them.
- **Stack:** every instance has one, its header `Name: message` and its first
  frame the code that constructed it; `Error.prepareStackTrace` and
  `Error.captureStackTrace` apply.
- **Native instances:** tunneled JSG errors, the serializer's
  `DataCloneError`, abort reasons (which also carry `retryable: true`) and the
  file system's errors are instances of the global `DOMException`, shaped as
  constructed ones.
- **Serialization:** name, message and stack (tag 8). Subclass instances come
  back as `DOMException`; other own properties, and own `name` or `message`
  properties shadowing the attributes, are not serialized. Tags 7 and 8 read
  back as `DOMException`s.
- **Crossing workers:** thrown from a fetch handler (tunneled through its
  `kj::Exception`) or over RPC, it arrives as a `DOMException` with its name
  and message and `remote: true`; returned over RPC, likewise, with RPC's
  disposer.

## Ledger

Where the TypeScript implementation differs from the C++, always toward
WebIDL. Each row is pinned on both sides with a `usingTsImpl` branch.

| # | Area | C++ | TypeScript | Test |
| --- | --- | --- | --- | --- |
| 1 | `name`, `message`, `code` | own enumerable read-only data properties of each instance | prototype accessors: `JSON.stringify()` gives `{}`, `Object.keys()` `[]`, `delete` has no effect | `api-surface.js`, `enumeration.js`, `errors.js` |
| 2 | `stack` | own enumerable accessor; deserialized, an own enumerable data property | V8's own non-enumerable accessor; deserialized, non-enumerable | `api-surface.js`, `stack.js`, `serialization.js` |
| 3 | `Error.isError()` | false | true: V8 asks the embedder about an API wrapper object (`IsolateBase::isJsApiWrapperNativeErrorCallback()`) | `errors.js` |
| 4 | `util.inspect()` | JSG's rendering of the own properties | an error's, as Node renders its DOMException; the prototype as an object whose getters are not called | `inspect.js` |
| 5 | Prototype symbols | JSG's `cloudflare:internal-class` and `kResourceTypeInspect` | `cloudflare:internal-class` only | `api-surface.js` |
| 6 | Attributes of something not a DOMException (`Object.create(DOMException.prototype)`, `String(DOMException.prototype)`) | `Error.prototype`'s | `TypeError` | `errors.js` |
| 7 | Legacy code constants | configurable, unless `spec_compliant_property_attributes` | not configurable | `api-surface.js` |
| 8 | Lone surrogates in the arguments | U+FFFD per byte of their WTF-8 | kept | `construction.js` |
| 9 | Stack of a subclass instance | includes the subclass constructor's frame | omits it, as for subclasses of `Error` | `stack.js` |
| 10 | Constructor's `prototype` | writable, unless `immutable_api_prototypes` | not writable | `api-surface.js` |
| 11 | `Symbol.toStringTag` | writable | not writable | `api-surface.js` |
| 12 | Calling without `new` | JSG's `TypeError` message | the language's | `construction.js` |
| 13 | `code` of `DOMStringSizeError`, `NoDataAllowedError`, `ValidationError` | 2, 6, 16 | 0: WebIDL's table of names has none of them, though their constants remain | `construction.js` |
