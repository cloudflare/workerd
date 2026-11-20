# navigator

An informal specification of the `navigator` global and its `Navigator`
class as implemented in workerd, kept in lockstep with the test suite in
this directory. **The tests are the normative artifact.** The cells run the
same modules (`navigator-modules.capnp`):

| Cell | Implementation | Flags the suite branches on |
| --- | --- | --- |
| `navigator-cpp` | `api/global-scope.{h,c++}` | those the variant's date gives: `enable_navigator_language`, `set_tostring_tag` |
| `navigator-storage-cpp` | the same | also `enable_web_file_system` (`--experimental`) |
| `navigator-opt-in-cpp` | the same | also `enable_web_file_system`, `spec_compliant_property_attributes` and `immutable_api_prototypes` (`--experimental`) |
| `navigator-absent-cpp` | the same | `no_global_navigator`: `absent.js` only |
| `navigator-ts`, `navigator-storage-ts`, `navigator-opt-in-ts`, `navigator-absent-ts` | `src/per_isolate/navigator.ts`, under the `TS_NAVIGATOR` autogate | as the C++ cell of the same name |

`which-impl.js` reads the flags; a cell's `USING_TS_IMPL` binding says which
implementation it means to run. The C++ cells have no `@all-autogates`
variant, since enabling every autogate enables `TS_NAVIGATOR`.

Every cell but the absent ones has three services: the suite (`main.js`); a
worker of its own (`first-read.js`) whose top level replaces built-ins
before the first read of `navigator`, which makes the instance, and which
then makes the changes to the instance that cannot be undone; and the sink
(`sink.js`), the other two's global outbound, which records the beacons.

`//src/workerd/api/tests:startup-snapshot-navigator-test` holds the
navigator across a startup snapshot; its `@all-autogates` variant runs the
TypeScript implementation through the snapshot pipeline.

The Web Platform Tests for the navigator are
`//src/wpt:html/webappapis/system-state-and-capabilities/the-navigator-object`,
and `//src/wpt:fs` reaches the file system through `navigator.storage`. The
`@all-autogates` variant of each runs the TypeScript implementation.

## Core semantics

- **The global:** `navigator` is a writable, enumerable, configurable own
  data property of the global, made on its first read and the same object
  from then on. `Navigator` is a writable, enumerable, configurable data
  property of the global scope's prototype, and under
  `spec_compliant_property_attributes` also a non-enumerable own property of
  the global. `no_global_navigator` leaves both out (`api-surface.js`,
  `absent.js`).
- **The constructor:** a function named `Navigator` of length 0, whose
  `prototype` is writable unless `immutable_api_prototypes`; assigning it
  changes what `instanceof` says of the navigator, not the navigator.
  Constructed,
  called or reached from a subclass, it throws `TypeError: Illegal
  constructor`, so the global is the only instance.
- **The prototype:** `sendBeacon`, then `constructor`, then JSG's marks
  (`Symbol.toStringTag` under `set_tostring_tag`, the internal-class marker,
  the inspect hook). No attribute is an accessor of the prototype.
- **The instance:** every attribute is an own, read-only, enumerable,
  configurable data property, in the reverse of the order in which
  `global-scope.h` registers them: `storage` (under
  `enable_web_file_system`), `languages` and `language` (under
  `enable_navigator_language`), `platform`, `hardwareConcurrency`,
  `userAgent`. So an object that inherits from the navigator reads them,
  and user code can redefine or delete them (`attributes.js`,
  `first-read.js`).
- **Values:** `userAgent` is `'Cloudflare-Workers'`, `hardwareConcurrency`
  1, `platform` `''`, `language` `'en'`. `languages` is a new, unfrozen
  `['en']` on every read, the descriptor's `value` included. `storage` is
  one `StorageManager` per navigator.
- **`sendBeacon(url, body)`:** checks, in order, that the receiver is the
  navigator itself (an object inheriting from it, or from the prototype, is
  not), that there is an argument, then converts `url` with ToString and
  `body` as a request body, where anything that is no body type is
  converted to a string. Within a request it schedules a POST that the
  request waits for and returns `true`; outside of one, after the same
  conversions, it returns `false`. A fetch that fails, as for an invalid
  URL, fails after `sendBeacon()` has returned (`send-beacon.js`,
  `top-level.js`).
- **Errors:** `TypeError`s with JSG's messages (`helpers.js`), whose stacks
  start at the caller.
- **Serialization:** `structuredClone(navigator)` throws a `DataCloneError`.
- **Pollution:** nothing the navigator does calls or reads anything user
  code can replace, its first read included (`pollution.js`,
  `first-read.js`).

## Ledger

Where the TypeScript implementation differs from the C++. Each row is pinned
on both sides with a `usingTsImpl` branch.

| # | Area | C++ | TypeScript | Test |
| --- | --- | --- | --- | --- |
| 1 | `Function.prototype.toString()` of `Navigator` and `sendBeacon` | `function Navigator() { [native code] }` | the function's source | `api-surface.js` |
| 2 | Code run by the conversion of `sendBeacon()`'s arguments (`toString()`, `Symbol.toPrimitive`) | called from the caller's frame | a frame of `workerd:per-isolate/navigator` in between, which the stack of an error made there shows, as does that of the error a failed beacon is logged with | `send-beacon.js` |
| 3 | Position of `Navigator` among the properties of the global scope's prototype, which `for...in` over the global follows | where `global-scope.h` registers it, after `TextEncoder` | after the properties JSG registers | `api-surface.js` |
| 4 | `Navigator.prototype` assigned (without `immutable_api_prototypes`) before the first read of `navigator` | the instance inherits from the object assigned | the instance inherits from the original prototype | `first-read.js` |
