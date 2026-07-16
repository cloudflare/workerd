# src/per_isolate/

Per-isolate JavaScript/TypeScript bootstrap: scripts that run synchronously
at context creation, before any user code. Gated by the
`per-isolate-javascript-bootstrap` autogate (config:
`workerd-autogate-per-isolate-javascript-bootstrap`). C++ entry point:
`src/workerd/io/per-isolate-bootstrap.c++`.

## EXECUTION MODEL

- Scripts are compiled as functions with a context-extension object — the
  pseudo-globals `require`, `module`, `exports`, `compatFlags`,
  `autogates`, `primordials`, `utils` are in scope but NOT on
  `globalThis` (see `per_isolate-env.d.ts`).
- Module system is bootstrap CommonJS: `require('webstreams/queue')` +
  `module.exports = {...}`. The `src/node/` ESM-only rule does NOT apply
  here. Circular requires are a FATAL startup error — keep modules
  acyclic (e.g., `webstreams/native` is a deliberate leaf).
- TypeScript `import type` / `export type` are used freely for type
  plumbing (fully erased; the loader only sees `module.exports`).
- `main.ts` installs the (TEMPORARY, dev-only) lazy `globalThis.streams`
  surface.
- Files are auto-discovered (`BUILD.bazel` and `tsconfig.json` both glob
  `**/*.ts`). No registration needed for new files.
- Local convention: no copyright headers in this directory's bootstrap
  scripts (deviation from the repo-wide new-file rule; keep consistent).

## CONVENTIONS

- **Primordials discipline**: no bare prototype lookups on builtins after
  bootstrap captures; no `for...of` over patchable iterables. Capture
  methods/getters at module scope via `uncurryThis`. See the
  **PRIMORDIALS USAGE** section below for the full guide.
- **JSG property capture trap**: readonly JSG properties live on the
  PROTOTYPE only under modern compat
  (`workers_api_getters_setters_on_prototype`, default-on 2022-01-31).
  Under older dates JSG uses `SetNativeDataProperty` on the INSTANCE
  template — an own native DATA property per instance; **no getter
  function exists anywhere to capture**. Use `captureJsgGetter` in
  the `primordials.ts` : prototype-accessor capture with a plain-read fallback
  (an own data property read never consults the patchable prototype chain).
  The bootstrap runs for every worker regardless of compat date — both layouts
  must work. JSG *methods* are always on the prototype.
- Internal cross-module surfaces are exported as a single namespace
  object (`internalsForPipe`, `nativeStreamInternals`), never
  re-exported to users.
- Internal class dispatch uses private-brand `in` checks, NEVER
  `instanceof` (classes reachable from the global have user-reachable
  `Symbol.hasInstance`).

## PRIMORDIALS USAGE

Bootstrap code shares an isolate with user code. After bootstrap runs,
user code can replace or patch any global constructor, prototype method,
or well-known symbol accessor. Primordials are pre-user-code captures of
built-in functions and constructors that remain trustworthy regardless of
subsequent mutation. The captures are in `primordials.ts`; the `primordials`
pseudo-global is injected into every bootstrap script's scope.

**What primordials guarantee — and what they do not.** Primordials are
an **internal correctness** mechanism, not a security sandbox. The hard
security boundary is the V8 isolate: separate address spaces, no shared
mutable state between tenants. Within a single isolate, primordials
ensure that the runtime's own implementation (bootstrap code, built-in
API polyfills, internal bookkeeping) continues to function correctly even
when user code mutates built-in prototypes — whether by accident (sloppy
polyfills, test mocks) or by design (legitimate metaprogramming).
Without primordials, a user doing `Array.prototype.push = () => {}` or
`Promise.prototype.then = badFn` could break internal runtime code that
calls those methods, leading to silent data corruption, spec-violating
behaviour, or crashes. Primordials prevent that class of failure.

### When primordials ARE required

Use captured references whenever bootstrap code operates on **built-in
objects whose prototypes are reachable by user code**:

- **Method calls on built-in types.** `array.push(v)` dispatches through
  `Array.prototype.push` — use `ArrayPrototypePush(array, v)`.
  This applies to Map, Set, Promise, ArrayBuffer, TypedArray, DataView,
  String, and every other built-in with a mutable prototype.
- **Constructor calls.** `new Map()` uses the global `Map` — use
  `new primordials.Map()` (or `new SafeMap()` when you also need safe
  method dispatch — see below).
- **Static methods.** `Promise.resolve(v)` uses the global `Promise` —
  use `PromiseResolve(v)`.
- **Iteration of internal collections.** `for...of` calls
  `[Symbol.iterator]()` on the target, which is patchable on built-in
  prototypes. For arrays, use `SafeArrayIterator` or index-based loops.
  For Map/Set, use `SafeMap`/`SafeSet` (whose `[Symbol.iterator]` is
  overridden) or iterate with captured methods and
  `SafeMapIterator`/`SafeSetIterator`.
- **Metadata reads on views at trust boundaries.** `view.buffer`,
  `view.byteOffset`, `view.byteLength`, `view.length` are prototype
  accessors that user code can shadow or redefine. Use the captured
  getters: `TypedArrayPrototypeGetBuffer(view)`,
  `TypedArrayPrototypeGetByteLength(view)`, etc. Same for DataView
  equivalents.
- **Type identification.** Never use `instanceof` or `.constructor` for
  type checks — both are user-controllable. Use
  `TypedArrayPrototypeGetSymbolToStringTag(value)` for typed arrays
  (returns the internal `[[TypedArrayName]]` or `undefined`) and
  private-brand `#field in obj` checks for internal classes.

### When primordials are NOT required

Primordials protect bootstrap code from mutations to **built-in
prototypes**. They are **not needed — and actively wrong — when the goal
is to invoke user-defined behaviour**:

- **User-provided iterables / async iterables.** When an API accepts an
  `Iterable` or `AsyncIterable` from user code (e.g.,
  `ReadableStream.from(userIterable)`), you MUST call the user's
  `[Symbol.iterator]()` / `[Symbol.asyncIterator]()` — that is the API
  contract. Using `SafeArrayIterator` would bypass the user's iterator
  and break semantics. Standard `for...of` / `for await...of` is
  correct here.
- **User-provided callbacks.** Callbacks passed by user code (e.g.,
  `underlyingSource.pull`, `transformer.transform`, strategy `size()`)
  are user-defined functions — call them normally.
- **User-provided objects generally.** When reading properties from
  user-provided objects (e.g., the `init` bag in `new Request(url,
  init)`), normal property access is correct — you are consuming the
  user's API surface, not a built-in prototype.
- **Spread / `Array.from` on user iterables.** `[...userIterable]` and
  `ArrayFrom(userIterable)` both invoke the user's iteration protocol,
  which is correct when the spec requires consuming a user iterable.

The bright line: **primordials protect built-in prototype chains from
third-party mutation**. If the object is user-provided and you are
intentionally invoking user-defined protocols, use normal JS operations.

### Captured constructors vs Safe wrappers

The exports include both raw captured constructors and Safe wrappers.
These are NOT interchangeable:

- `primordials.Map` (= `MapCtor`): the original `Map` constructor,
  captured before user code runs. `new primordials.Map()` creates a
  real Map, but **method calls on it** (`.get()`, `.set()`, etc.) still
  dispatch through `Map.prototype` — which user code can patch.
- `SafeMap`: extends `MapCtor` with every method overridden to dispatch
  through captured primordials. `new SafeMap()` is safe for both
  construction and subsequent method calls. Same for `SafeSet`,
  `SafeWeakMap`, `SafeWeakSet`.

Use captured constructors (`primordials.Map`) only when you need `new`
but will access the instance exclusively through captured uncurried
methods (e.g., `MapPrototypeGet(map, key)`). Use Safe wrappers when you
want normal `map.get(key)` call syntax with pollution resistance.

### Promise patterns

- **`SafePromise`**: species-protected; `.then()` / `.catch()` /
  `.finally()` use captured methods and `Symbol.species` is pinned.
  Static helpers (`SafePromise.resolve`, `.reject`, `.all`, etc.) are
  bound to `SafePromise`. Use for **internal-only** promise chains
  where species hijacking would be a concern.
- **`PromiseResolve` / `PromiseReject` / `PromiseWithResolvers`**:
  captured statics bound to the original `Promise` constructor. Use for
  **user-facing** promises — the returned promise is a regular
  `Promise`, which is what spec algorithms and user code expect.
- **`PromisePrototypeThen` / `PromisePrototypeCatch` /
  `PromisePrototypeFinally`**: captured prototype methods. Use when
  chaining on an existing promise (internal or external) without
  relying on the prototype chain.

### Never expose primordials to user code

Primordial captures, Safe\* classes, and captured constructors are
**strictly internal**. Never assign them as properties on any object
reachable by user code. If user code can reach a Safe\* class instance,
it can traverse `Object.getPrototypeOf()` to reach the Safe class and
then the original captured constructor — potentially mutating it and
affecting all code that references it.

Consequences:
- Never return a `SafePromise` to user code — wrap with
  `PromiseResolve()` to return a regular `Promise`.
- Never store a Safe\* wrapper on a user-visible object property.
- Never expose captured constructors (e.g., `primordials.Map`) as API
  return values or properties.

The `module.exports` object from `primordials.ts` is `ObjectFreeze`-d
to prevent accidental mutation by downstream bootstrap scripts.

## ANTI-PATTERNS

- **NEVER** assume a JSG readonly property has a capturable getter (see
  the capture trap above).
- **NEVER** add a runtime require cycle between bootstrap scripts.

See also `webstreams/AGENTS.md` for streams-specific architecture and
anti-patterns.
