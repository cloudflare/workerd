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
| `dom-exception-v8`, `dom-exception-legacy-flags-v8`, `dom-exception-opt-in-v8` | V8's (`patches/v8/2026-11-23-Implement-DOMException-and-QuotaExceededError.patch`), under the `V8_DOM_EXCEPTION` autogate | as the C++ cell of the same name |

`which-impl.js` reads the flags; a cell's `USING_V8_IMPL` binding says which
implementation it means to run. The C++ cells have no `@all-autogates`
variant, since enabling every autogate enables `V8_DOM_EXCEPTION`. Every
cell needs `--experimental`, for
`service_binding_extra_handlers` (crafted payloads through
`Fetcher.queue()`), `enable_web_file_system` and `enable_abortsignal_rpc`.

What JavaScript cannot see is pinned in C++: the bytes a DOMException
serializes to, what reading tags 7, 8 and 24 creates and which data reading
tag 24 rejects, the `kj::Exception` it tunnels as and `reportError()`'s
description, and the bytes V8's QuotaExceededError serializes to and what
reading them does with either implementation, in
`api/dom-exception-bootstrap-test.c++`, which runs with the per-isolate
bootstrap; its `@all-autogates` variant runs them against V8's
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
  `DataCloneError`, the default abort and timeout reasons (which also carry
  `retryable: true`) and the file system's errors are instances of the global
  `DOMException`, shaped as constructed ones.
- **Serialization:** name, message and stack; what else is written, and how
  the stack is, differ (ledger #16). Subclass instances come back as
  `DOMException`, but for QuotaExceededError's (ledger #14). Tags 7, 8 and 24
  read back as `DOMException`s. Received over RPC, one has the local stack,
  not the sender's.
- **Crossing workers:** thrown from a fetch handler (tunneled through its
  `kj::Exception`) or over RPC, it arrives as a `DOMException` with its name
  and message and `remote: true`; returned over RPC, likewise, with RPC's
  disposer.

## Ledger

Where V8's implementation differs from the C++, always toward WebIDL. Each
row is pinned on both sides with a `usingV8Impl` branch.

| # | Area | C++ | V8 | Test |
| --- | --- | --- | --- | --- |
| 1 | `name`, `message`, `code` | own enumerable read-only data properties of each instance | prototype accessors: `JSON.stringify()` gives `{}`, `Object.keys()` `[]`, `delete` has no effect | `api-surface.js`, `enumeration.js`, `errors.js` |
| 2 | `stack` | own enumerable accessor; deserialized, an own enumerable data property | V8's own non-enumerable accessor; deserialized, non-enumerable | `api-surface.js`, `stack.js`, `serialization.js` |
| 3 | `Error.isError()` | false | true: its instances are native errors | `errors.js` |
| 4 | `util.inspect()` | JSG's rendering of the own properties | an error's, as Node renders its DOMException; the prototype as an object whose getters are not called | `inspect.js` |
| 5 | Prototype symbols | JSG's `cloudflare:internal-class` and `kResourceTypeInspect` | none but `Symbol.toStringTag` | `api-surface.js` |
| 6 | Attributes of something not a DOMException (`Object.create(DOMException.prototype)`, `String(DOMException.prototype)`) | `Error.prototype`'s | `TypeError` | `errors.js` |
| 7 | Legacy code constants | configurable, unless `spec_compliant_property_attributes` | not configurable | `api-surface.js` |
| 8 | Lone surrogates in the arguments | U+FFFD per byte of their WTF-8 | kept | `construction.js` |
| 9 | Stack of a subclass instance | includes the subclass constructor's frame | omits it, as for subclasses of `Error` | `stack.js` |
| 10 | Constructor's `prototype` | writable, unless `immutable_api_prototypes` | not writable | `api-surface.js` |
| 11 | `Symbol.toStringTag` | only under `set_tostring_tag`, and writable | always, and not writable | `api-surface.js`, `errors.js`, `inspect.js` |
| 12 | Calling without `new` | JSG's `TypeError` message | the language's | `construction.js` |
| 13 | `code` of `DOMStringSizeError`, `NoDataAllowedError`, `ValidationError` | 2, 6, 16 | 0: WebIDL's table of names has none of them, though their constants remain | `construction.js` |
| 14 | `QuotaExceededError` (WebIDL section 2.8.3) | none; reading a serialized one throws `DataCloneError` | WebIDL's subclass, with `quota` and `requested`. It serializes as tag 25, as a `DOMException` does as tag 24 (ledger #16), with its amounts too, and so does a subclass's instance. Thrown across workers, it tunnels as a `DOMException` named `QuotaExceededError` | `quota-exceeded-error.js` |
| 15 | Placement of the globals | on the global scope's prototype, and under `spec_compliant_property_attributes` also an own non-enumerable property of the global | an own non-enumerable property of the global only, whatever the flags | `api-surface.js`, `quota-exceeded-error.js` |
| 16 | Serialization | tag 8: the stack converted to a string, empty if it is undefined or missing, and no other own properties, nor own `name` or `message` properties that shadow the attributes. Reading tag 24, it drops the other properties, and its stack is the value received, or undefined if none was | tag 24: its own stack as whatever value it holds, or none, and, with `enhanced_error_serialization`, its other own string-keyed properties, those that shadow the attributes included, as for other errors. A value that refers back to the instance, in its stack or in those properties, can't be read: V8 throws `Error: Unable to deserialize cloned data.` | `serialization.js`, `tunneling.js` |
