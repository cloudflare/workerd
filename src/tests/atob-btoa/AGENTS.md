# atob() and btoa()

An informal specification of the globals `atob()` and `btoa()` as
implemented in workerd, kept in lockstep with the test suite in this
directory. **The tests are the normative artifact.** The cells run the same
modules (`atob-btoa-modules.capnp`):

| Cell | Implementation | Flags the suite branches on |
| --- | --- | --- |
| `atob-btoa-cpp` | `api/global-scope.{h,c++}` | none |
| `atob-btoa-opt-in-cpp` | the same, with the dateless WebIDL opt-in | `spec_compliant_property_attributes` (`--experimental`) |
| `atob-btoa-ts`, `atob-btoa-opt-in-ts` | `src/per_isolate/atob-btoa.ts`, under the `TS_ATOB_BTOA` autogate | as the C++ cell of the same name |

`which-impl.js` reads the flag; a cell's `USING_TS_IMPL` binding says which
implementation it means to run. The C++ cells have no `@all-autogates`
variant, since enabling every autogate enables `TS_ATOB_BTOA`. The
TypeScript cells' `@all-autogates` variant runs them with the TypeScript
DOMException (`TS_DOM_EXCEPTION`) too.

The Web Platform Tests for both functions are `//src/wpt:html/webappapis/atob`,
whose `@all-autogates` variant runs the TypeScript implementation.

## Core semantics

- **Placement:** writable, enumerable, configurable data properties of the
  global scope's prototype (`ServiceWorkerGlobalScope.prototype`), `btoa`
  before `atob`, whatever the flags; not own properties of the global
  (`api-surface.js`).
- **Shape:** ordinary functions named `atob` and `btoa`, not constructors,
  with no `prototype`. `length` is 0, or 1 under
  `spec_compliant_property_attributes`.
- **Receiver:** `undefined`, `null` or the global; anything else throws
  JSG's illegal-invocation `TypeError`, before the argument is looked at
  (`conversions.js`).
- **Argument:** required: without one, a `TypeError` naming parameter 1.
  Otherwise converted with ToString, so `undefined` is `'undefined'`, a
  Symbol throws `TypeError`, and what an object's conversion throws
  propagates as it is. Further arguments are ignored.
- **`atob()`:** WHATWG Infra's forgiving-base64 decode: ASCII whitespace is
  removed, padding is optional but must be right when present, the bits a
  final character leaves over are discarded, and a length of 1 modulo 4 or
  any other character fails (`decoding.js`). The result has one code unit
  per byte.
- **`btoa()`:** RFC 4648 base64, padded, of the bytes the argument's code
  units are; a code unit above 0xFF fails (`encoding.js`).
- **Errors:** a failure throws a new `DOMException` named
  `InvalidCharacterError` (code 5), with workerd's messages (`helpers.js`),
  shaped as one that script constructs. The stack of every error the
  functions make starts at the caller (`errors.js`).
- **Pollution:** neither function calls or reads anything that user code
  can replace (`pollution.js`).

## Ledger

Where the TypeScript implementation differs from the C++. Each row is pinned
on both sides with a `usingTsImpl` branch.

| # | Area | C++ | TypeScript | Test |
| --- | --- | --- | --- | --- |
| 1 | `Function.prototype.toString()` | `function atob() { [native code] }` | the function's source | `api-surface.js` |
| 2 | Code run by the argument's conversion (`toString()`, `Symbol.toPrimitive`) | called from the caller's frame | a frame of `workerd:per-isolate/atob-btoa` in between, which the stack of an error thrown there shows | `errors.js` |
| 3 | Position among the properties of the global scope's prototype, which `for...in` over the global follows | where `global-scope.h` registers them, before `setTimeout` | after the properties JSG registers | `api-surface.js` |
