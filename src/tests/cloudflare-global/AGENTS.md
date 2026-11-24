# cloudflare-global

An informal specification of the `Cloudflare` global and its
`compatibilityFlags` as implemented in workerd, kept in lockstep with the
test suite in this directory. **The tests are the normative artifact.** The
cells run the same modules (`cloudflare-global-modules.capnp`):

| Cell | Implementation | Flags |
| --- | --- | --- |
| `cloudflare-global-cpp` | `api/global-scope.{h,c++}` | those the variant's date gives, so `set_tostring_tag` and the values of every dated flag differ between `@` and `@all-compat-flags` |
| `cloudflare-global-opt-in-cpp` | the same | also `set_tostring_tag` at either date, `spec_compliant_property_attributes`, `immutable_api_prototypes` and `enable_web_file_system` (`--experimental`) |
| `cloudflare-global-ts`, `cloudflare-global-opt-in-ts` | `src/per_isolate/cloudflare-global.ts`, under the `TS_CLOUDFLARE_GLOBAL` autogate | as the C++ cell of the same name |

`which-impl.js` reads the flags; a cell's `USING_TS_IMPL` binding says which
implementation it means to run, and `NAMED_FLAGS` which flags its config
names. The C++ cells have no `@all-autogates` variant, since enabling every
autogate enables `TS_CLOUDFLARE_GLOBAL`.

Every cell has two services: the suite (`main.js`), and a worker of its own
(`first-read.js`) whose top level replaces built-ins before the first read
of `Cloudflare`, which makes the object, and which then makes the changes
to the object that cannot be undone. The runtime's own Node.js modules read
`Cloudflare` as they load, so that worker imports `node:assert` only in its
test; in the `@all-compat-flags` variants the runtime has still read the
global before the worker's top level runs, and there the worker checks only
what it finds.

The names the flags object must list, and their order, come from the schema
itself: `io/compatibility-date.capnp` is a text module of both workers,
which read each field's number and enable-flag from it.

`//src/workerd/api/tests:startup-snapshot-cloudflare-global-test` holds the
object and its flags across a startup snapshot; its `@all-autogates` variant
runs the TypeScript implementation through the snapshot pipeline. No Web
Platform Test reads the global.

## Core semantics

- **The global:** `Cloudflare` is a writable, enumerable, configurable own
  data property of the global, whatever the flags, made on its first read
  and the same object from then on. User code can assign it, delete it or
  declare its own. Nothing names its constructor (`shape.js`).
- **The object:** inherits from a prototype of its own, which inherits from
  `Object.prototype`. Its one own property is `compatibilityFlags`, a
  read-only, enumerable, configurable data property. The object is
  extensible, and `structuredClone()` of it throws a `DataCloneError`.
- **The prototype:** `constructor`, then JSG's marks (`Symbol.toStringTag`
  under `set_tostring_tag`, the internal-class marker, the inspect hook).
- **The constructor:** reachable only as `Cloudflare.constructor`: a
  function named `Cloudflare` of length 0, whose `prototype` is writable
  unless `immutable_api_prototypes`; assigning it changes what `instanceof`
  says of the object, not the object. Constructed, called or reached from a
  subclass, it throws `TypeError: Illegal constructor`, whose stack starts
  at the caller.
- **`compatibilityFlags`:** an object without a prototype, the same on every
  read, that maps the name of every compatibility enable-flag, in the order
  of the fields' numbers in the schema, to `true` if the flag is set for the worker and
  `false` if not. Disable-flags are not listed, experimental flags are. Each
  property is read-only and non-configurable and the object is not
  extensible, so it is both sealed and frozen (`flags.js`).
- **Loading:** the TypeScript implementation loads at the first read of the
  global (`src/per_isolate/main.ts`), so a Worker that does not use it does
  not pay for it.
- **Pollution:** making the object and its flags calls or reads nothing user
  code can replace, but for what loading the TypeScript implementation calls
  (ledger row 3, `first-read.js`), nor does the constructor (`shape.js`).

## Ledger

Where the TypeScript implementation differs from the C++. Each row is pinned
on both sides with a `usingTsImpl` branch.

| # | Area | C++ | TypeScript | Test |
| --- | --- | --- | --- | --- |
| 1 | `Function.prototype.toString()` of `Cloudflare.constructor`, and the error messages that quote it, such as that of assigning its `prototype` under `immutable_api_prototypes` | `function Cloudflare() { [native code] }` | the function's source, which V8 abbreviates in a message | `shape.js` |
| 2 | Position of `Cloudflare` among the global's own properties, which `for...in` over the global follows | where `global-scope.h` registers it, next to `performance` | after the properties JSG registers | `shape.js` |
| 3 | User code that replaced the global `Object.defineProperty` before the first read of `Cloudflare` | not called | called as the module loads, with the module's initial `exports` object (empty, and used by nothing else), `'__esModule'` and `{ value: true }`; what it throws fails that read, and the next read loads the module again | `first-read.js` |
