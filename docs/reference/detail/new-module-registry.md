# New Module Registry (`jsg::modules::ModuleRegistry`)

Reference for reasoning about the new module registry implementation, how it
interfaces with V8's module APIs, and how modules flow from config through
compilation to evaluation. For V8 module internals, see
[v8-module-internals.md](v8-module-internals.md). For the legacy implementation,
see [legacy-module-registry.md](legacy-module-registry.md).

## Source Files

| File                                 | Role                                                                                                              |
| ------------------------------------ | ----------------------------------------------------------------------------------------------------------------- |
| `src/workerd/jsg/modules-new.h`      | `Module`, `ModuleBundle`, `ModuleRegistry` classes; `ResolveContext`; builder APIs; `IsolateModuleRegistry` (fwd) |
| `src/workerd/jsg/modules-new.c++`    | All implementation: `EsModule`, `SyntheticModule`, `IsolateModuleRegistry`, V8 callbacks, bundle builders         |
| `src/workerd/jsg/observer.h`         | `ResolveObserver` — metrics/tracing hooks for module resolution                                                   |
| `src/workerd/jsg/url.h`              | `jsg::Url` — WHATWG URL wrapper (ada-url); used as specifier keys throughout                                      |
| `src/workerd/jsg/resource.h:1913–22` | Context creation: picks legacy vs new registry based on `NewContextOptions`                                       |
| `src/workerd/jsg/jsg.c++:357–395`    | `Lock::resolveModule` / `Lock::resolveInternalModule` dispatches between legacy and new                           |
| `src/workerd/jsg/setup.h:291–297`    | `IsolateBase::isUsingNewModuleRegistry()` flag                                                                    |
| `src/workerd/io/worker-modules.h`    | `newWorkerModuleRegistry<TypeWrapper>` — creates `ModuleRegistry` from worker config                              |
| `src/workerd/api/modules.h`          | `registerModules` (legacy), `registerBuiltinModules` (new) — registers all built-in API modules                   |
| `src/workerd/server/workerd-api.c++` | `WorkerdApi::newWorkerdModuleRegistry` — workerd-specific new registry construction                               |

## Design Goals and Key Differences from Legacy

The new module registry was designed to address several limitations of the legacy
`ModuleRegistryImpl<TypeWrapper>`:

1. **Thread-safe and shareable across isolate replicas.** The `ModuleRegistry` is
   `kj::AtomicRefcounted` and protected by `kj::MutexGuarded`. Multiple
   `Worker::Isolate` replicas can share a single `ModuleRegistry` instance. The
   legacy registry was per-isolate with no sharing.

2. **URL-based specifiers.** All specifiers are handled as `jsg::Url` objects
   (WHATWG-compliant, backed by ada-url). The legacy registry used `kj::Path`.
   This enables proper protocol-based dispatch (`node:`, `cloudflare:`, `file:`)
   and correct relative resolution.

3. **Fully lazy compilation and evaluation.** No modules are compiled during
   `ModuleRegistry` construction. Both ESM compilation and synthetic module
   evaluation happen on first import. The legacy registry eagerly compiled all
   worker bundle ESM modules at startup.

4. **Cross-isolate compile cache.** ESM modules cache their V8 compiled bytecode
   in a `kj::MutexGuarded` field on the `Module` object itself. When a second
   isolate imports the same module, the cached bytecode is consumed, skipping
   recompilation. The legacy registry had no compile cache sharing.

5. **Separation of definition from instantiation.** `Module` objects are
   isolate-independent definitions. `IsolateModuleRegistry` holds the per-isolate
   V8 handles. This clean split enables the sharing model.

6. **`import.meta` support.** The new registry provides `import.meta.url`,
   `import.meta.main`, and `import.meta.resolve()` on all ESM modules. The legacy
   registry had no `import.meta` support.

7. **Modular bundle architecture.** The `ModuleBundle` abstraction allows
   composing registries from multiple independent sources (worker bundle, builtins,
   internal-only, fallback) without coupling them.

## Architecture Overview

The system has a two-layer design: a shared, isolate-independent layer
(`ModuleRegistry` + `ModuleBundle` + `Module`) and a per-isolate layer
(`IsolateModuleRegistry`).

### Shared Layer (owned by `Worker::Script`, thread-safe)

```
ModuleRegistry (kj::AtomicRefcounted)
  |-- observer: const ResolveObserver&     (metrics hooks)
  |-- bundleBase: const jsg::Url&          (e.g. "file:///bundle/")
  |-- schemaLoader: capnp::SchemaLoader    (for capnp modules)
  |-- maybeEvalCallback: EvalCallback      (ensures eval outside IoContext)
  +-- impl: kj::MutexGuarded<Impl>
        +-- bundles: FixedArray<Array<Own<ModuleBundle>>, 4>
              [kBundle]      -> worker bundle modules
              [kBuiltin]     -> runtime builtins importable by user code
              [kBuiltinOnly] -> internal-only modules
              [kFallback]    -> fallback service (local dev only)
```

### Per-Isolate Layer (owned by `JsContext`, destroyed with context)

```
IsolateModuleRegistry
  |-- inner: const ModuleRegistry&         (back-reference to shared registry)
  |-- observer: const CompilationObserver& (compilation metrics)
  |-- instantiations: kj::Table<Entry>     (one row per live v8::Module)
  |     Each Entry:
  |       |-- key: HashableV8Ref<v8::Module>  (V8 module handle)
  |       |-- id: Url                         (specifier incl. query/fragment;
  |       |                                    this is the instance's import.meta.url)
  |       +-- module: const Module&           (back-ref to definition)
  |     Indices:
  |       |-- HashIndex<EntryCallbacks>     -- by v8::Module identity
  |       +-- HashIndex<InstanceCallbacks>  -- by (specifier URL, definition)
  +-- resolutions: HashMap<SpecifierContext, const Module*>
        maps (context type, specifier URL) -> the definition it resolves to
```

The two-level split encodes the module-identity rules the registry guarantees:

- **Query/fragment-distinct specifiers produce distinct instances**, each with
  its own `import.meta.url` (`import('./foo?a') !== import('./foo?b')`), per the
  HTML module-map model.
- **The same specifier resolved through different context types shares one
  instance when it resolves to the same definition** — e.g.
  `process.getBuiltinModule('cloudflare:sockets')` is reference-equal to the
  namespace obtained via `import()`, and a builtin imported by both user code
  and other builtins remains a per-isolate singleton (module-level state is
  never duplicated).
- **The same specifier resolved to different definitions yields distinct
  instances** — a worker-bundle module shadowing a builtin name coexists with
  the real builtin, and `PUBLIC_BUILTIN` resolution never observes the shadow.

There is deliberately no index keyed by URL alone: with shadowing, one URL can
map to multiple rows, so a unique URL index would collide.

## Module Types

### `Module::Type` enum

| Type           | Value | Purpose                                                        |
| -------------- | ----- | -------------------------------------------------------------- |
| `BUNDLE`       | 0     | User worker modules (from config)                              |
| `BUILTIN`      | 1     | Runtime-provided, importable by user code (e.g. `node:buffer`) |
| `BUILTIN_ONLY` | 2     | Runtime-internal, only importable by other builtins            |
| `FALLBACK`     | 3     | Dynamically resolved from fallback service (local dev only)    |

### `Module::Flags` bitmask

| Flag   | Bit  | Meaning                                                            |
| ------ | ---- | ------------------------------------------------------------------ |
| `NONE` | 0x00 | No flags                                                           |
| `MAIN` | 0x01 | `import.meta.main = true` (worker entrypoint)                      |
| `ESM`  | 0x02 | ECMAScript module (vs synthetic)                                   |
| `EVAL` | 0x04 | Requires evaluation outside IoContext (deferred to `EvalCallback`) |
| `WASM` | 0x08 | WebAssembly module                                                 |

### `Module::ContentType` enum

Used to validate import attributes (e.g. `with { type: 'json' }`). Set at
module construction time.

| ContentType | Meaning                                              |
| ----------- | ---------------------------------------------------- |
| `NONE`      | No specific content type (ESM, CJS, builtin objects) |
| `JSON`      | JSON module                                          |
| `TEXT`      | Text module                                          |
| `DATA`      | Data/binary module                                   |
| `WASM`      | WebAssembly module                                   |

## Module Subclasses

`Module` is an abstract base class. Two concrete implementations exist
(both file-private in `modules-new.c++`):

### `EsModule` — ECMAScript Modules

```
EsModule extends Module {
  source:        kj::OneOf<kj::ArrayPtr<const char>, kj::Arc<OwnedAscii>> // UTF-8
  encodedSource: kj::Lazy<EncodedSource>                         // V8-compatible encoding (shared)
  cachedData:    MutexGuarded<Maybe<Own<CachedData>>>            // Cross-isolate compile cache
}
```

- `getDescriptor()`: Encodes the source (see below), then compiles it via
  `v8::ScriptCompiler::CompileModule`. Checks the `cachedData` shared cache first
  (shared lock for reading, exclusive lock for writing). On cache miss, generates
  and stores bytecode after compilation.

#### Source Encoding (`EncodedSource`)

V8 has no internal UTF-8 string representation — external source strings must be
one-byte (Latin-1) or two-byte (UTF-16). Worker bundle sources arrive as UTF-8
bytes, so each module's source is encoded once, lazily, on first compile
(`kj::Lazy` handles cross-thread once-init) and the result is shared by every
isolate that compiles the module:

| Source content                     | Representation                                   |
| ---------------------------------- | ------------------------------------------------ |
| Pure ASCII (the common case)       | Zero-copy one-byte external over static or Arc-owned storage |
| Non-ASCII, all code points ≤ U+00FF | One-time UTF-8→Latin-1 transcode into Arc-owned storage       |
| Anything else (CJK, emoji, ...)    | One-time UTF-8→UTF-16 transcode into Arc-owned storage        |

Invalid UTF-8 sequences are replaced with U+FFFD (via `kj::encodeUtf16`),
matching `v8::String::NewFromUtf8`'s tolerance and the legacy registry. The
encoding choice is a pure function of the source bytes, so all replicas agree
and the shared compile cache stays consistent.
- `evaluate()`: Calls `v8::Module::Evaluate()`. For modules with `Flags::EVAL`,
  delegates to the `Evaluator` to run evaluation outside the IoContext.
- Always has `Flags::ESM | Flags::EVAL` set.

#### Compile Cache Flow

```
First isolate compiles module:
  1. lockShared(cachedData) -> empty -> no cached data
  2. CompileModule(source) -> v8::Module
  3. lockExclusive(cachedData) -> still empty?
     -> CreateCodeCache(module) -> store Own<CachedData>

Second isolate compiles same module:
  1. lockShared(cachedData) -> has data -> copy bytes
     -> kConsumeCodeCache -> CompileModule(source, cache)
  2. No exclusive lock needed (cache already populated)
```

### `SyntheticModule` — All Non-ESM Modules

```
SyntheticModule extends Module {
  callback:     EvaluateCallback    // Sets exports during evaluation
  namedExports: Array<String>       // Declared export names (besides "default")
}
```

- `getDescriptor()`: Calls `v8::Module::CreateSyntheticModule` with the declared
  export names (always includes `"default"` plus any `namedExports`).
- `evaluate()`: Ensures the module is instantiated, optionally delegates to the
  `Evaluator` (for `Flags::EVAL` modules), then calls `module->Evaluate()` to
  enter V8's status machine. V8 calls back into `evaluationSteps` which calls
  `actuallyEvaluate()`.
- `actuallyEvaluate()`: Creates a resolved Promise, calls the `callback` which
  uses the `ModuleNamespace` helper to set exports via
  `SetSyntheticModuleExport`. This is always invoked via V8's evaluation steps
  callback, never called directly from external code.

### Static `evaluationSteps` Callback

All synthetic modules share a single static `evaluationSteps` function, registered
as the V8 `SyntheticModuleEvaluationSteps`. When V8 calls it:

1. Gets the `IsolateModuleRegistry` from context embedder data.
2. Looks up the `Entry` by v8::Module identity (hash-indexed, O(1) — unlike the
   legacy registry's O(n) linear scan).
3. Calls `actuallyEvaluate()` on the found module — not `evaluate()`, to avoid
   reentry into `module->Evaluate()`.

This design ensures V8 always manages the status transitions (`kEvaluating` →
`kEvaluated` or `kErrored`) regardless of whether evaluation was initiated by
V8 (static import) or by our code (dynamic import, require). The `evaluate()`
method is the external entry point that goes through V8; `evaluationSteps` is
V8's callback entry point that goes directly to the work.

## ModuleBundle — Sources of Modules

A `ModuleBundle` is an immutable collection of module definitions. The
`ModuleRegistry` composes multiple bundles and searches them in priority order.

### `StaticModuleBundle` (file-private)

The default bundle type, created by `Builder::finish()`. Contains:

- `modules`: `HashMap<Url, ResolveCallback>` — URL to factory
- `aliases`: `HashMap<Url, Url>` — alias to target
- `cache`: `HashMap<Url, Own<Module>>` — resolved modules (populated on first lookup)

Each `ResolveCallback` is a factory `(ResolveContext) -> Maybe<OneOf<String, Own<Module>>>`.
It returns either a `Module` (resolved), a redirect string, or `kj::none` (not found).

### `FallbackModuleBundle` (file-private)

Used only for local development (`workerd` binary). Contains a single
`ResolveCallback` that calls an external service. Caches results in internal
`storage` and `aliases` maps.

### `BundleBuilder` — Building Worker Bundle Modules

`BundleBuilder` creates `BUNDLE`-type modules. Module names are processed relative
to the `bundleBase` URL (typically `file:///bundle/`):

```cpp
BundleBuilder builder(bundleBase);
builder.addEsmModule(
    "index.js", kj::arc<OwnedAscii>(kj::mv(source)), Flags::ESM | Flags::MAIN);
builder.addSyntheticModule("data.json", Module::newJsonModuleHandler(jsonData));
builder.addWasmModule("module.wasm", wasmBytes);
auto bundle = builder.finish();
```

The `kj::Arc<OwnedAscii>` overload must be used for ordinary worker source. The
`ArrayPtr` overload is reserved for compiled-in strings with static process
lifetime because V8 can retain an external source string after compilation.

Name normalization (`normalizeModuleName`):

1. Parse name as URL relative to `bundleBase`.
2. Normalize path (remove `.`/`..`, normalize percent-encoding).
3. Strip fragments and query parameters.
4. Validate: must be subordinate to `bundleBase`, no `cloudflare:` or `workerd:` protocol.

### `BuiltinBuilder` — Building Runtime Modules

`BuiltinBuilder` creates `BUILTIN` or `BUILTIN_ONLY` modules. Specifiers must be
absolute URLs with non-`file:` protocols:

```cpp
BuiltinBuilder builder(BuiltinBuilder::Type::BUILTIN);
builder.addEsm("node:buffer"_url, NODE_BUFFER_SOURCE);
builder.addSynthetic("node:path"_url, pathCallback);
builder.addObject<CryptoModule, TypeWrapper>("node:crypto"_url);
auto bundle = builder.finish();
```

## Resolution Flow

### Lookup Priority by Context Type

When `ModuleRegistry::lookup()` is called, it acquires an exclusive lock on the
`MutexGuarded<Impl>` and searches bundles in order:

| `ResolveContext::Type` | Search order                     | Use case                                   |
| ---------------------- | -------------------------------- | ------------------------------------------ |
| `BUNDLE`               | kBundle -> kBuiltin -> kFallback | User code imports                          |
| `BUILTIN`              | kBuiltin -> kBuiltinOnly         | Builtin code importing other builtins      |
| `BUILTIN_ONLY`         | kBuiltinOnly only                | Internal modules importing other internals |
| `PUBLIC_BUILTIN`       | kBuiltin only                    | `process.getBuiltinModule()` and similar   |

### Complete Resolution Sequence (Static Import)

```
User code: import { foo } from './bar.js'

1. V8 calls resolveModuleCallback<false>(context, specifier, attrs, referrer)

2. Look up referrer in IsolateModuleRegistry by v8::Module identity
   -> Determines referrer's type (BUNDLE/BUILTIN/BUILTIN_ONLY)
   -> Gets referrer's URL for relative resolution

3. Normalize specifier:
   a. If Node.js compat enabled, check for bare node specifier -> prefix "node:"
   b. Check for node:process redirect (v1 vs v2)
   c. Resolve specifier relative to referrer URL -> absolute URL
   d. Normalize path (percent-encoding normalization)

4. Build ResolveContext { type, source=STATIC_IMPORT, normalizedSpecifier, referrer }

5. IsolateModuleRegistry::resolve(js, context)
   a. findResolved(context): resolutions map hit by (type, URL) ->
      instantiations row by (URL, definition) -> return v8::Module handle
   b. Cache miss -> call resolveWithCaching(js, context)
      i.   Strip query params and fragments from specifier
      ii.  Call inner.lookup(innerContext)  [acquires MutexGuarded exclusive lock]
      iii. ModuleRegistry::lookupImpl searches bundles in priority order
      iv.  Each bundle's lookup() checks aliases, cache, then resolve callbacks
      v.   If redirect (alias): restart with the new specifier (chains followed
           to any depth, cycles detected via a visited-specifier set)
      vi.  Record (type, URL) -> definition in the resolutions map
      vii. If an instantiation already exists for (URL, definition) — e.g. the
           same builtin resolved through another context type — reuse its
           v8::Module; otherwise call module.getDescriptor(js, observer) and
           insert a new Entry{v8Module, id, module}
   c. Return v8::Module handle from the Entry

6. V8 receives the v8::Module and continues PrepareInstantiate
```

### Dynamic Import Flow

```
User code: const mod = await import('./bar.js')

1. V8 calls dynamicImport(context, host_options, resource_name, specifier, attrs)

2. Parse import attributes (throws for unrecognized keys; `type` validated
   against the resolved module's ContentType)

3. Parse referrer from resource_name. Modules created by this registry always
   use their canonical URL as the ScriptOrigin name (including CJS-style eval
   functions), so this parse succeeds for them. Scripts with non-URL origin
   names (service-worker mains, eval'd code) fall back to bundleBase, and the
   referrer lookup below then fails with a clear "Referring module not found"
   TypeError.

4. Normalize specifier (same as static: node: prefix, process redirect, URL resolve)

5. IsolateModuleRegistry::dynamicResolve(js, normalizedSpec, referrer, rawSpec, sourcePhase)
   a. Identify the referrer: probe the resolutions map for the referrer URL
      across context types in resolution-priority order (BUNDLE first). V8
      supplies only the URL string, and with shadowing one URL can have entries
      under multiple types, so referrer typing is a policy choice favoring the
      bundle module.
   b. Build ResolveContext { type from referrer's module, source=DYNAMIC_IMPORT, ... }
   c. findResolved or resolveWithCaching (same as static)
   d. Call module.evaluate(js, v8Module, observer, maybeEvaluate) -> Promise
   e. Chain: .then(namespace -> resolve Promise with module namespace)

6. Return Promise to V8

Note: a dynamically imported module whose top-level await never settles leaves
the import() promise pending indefinitely (standard ESM semantics, subject to
normal request hang detection). The legacy registry instead throws an eager
"Top-level await in module is unsettled." error, a deviation tied to its
evaluate-within-one-drain model.
```

### `require()` Flow

```
CJS code: const foo = require('./bar.js')

1. IsolateModuleRegistry::require(js, context, option)
   Options: RETURN_EMPTY (kj::none instead of throw on not-found),
   NO_TOP_LEVEL_AWAIT (always set for Source::REQUIRE, per Node's
   require(esm)), UNWRAP_DEFAULT (see "require() return value semantics"),
   REQUIRE_ESM (entry-point resolution: rejects non-ESM modules with the
   legacy-identical "Main module must be an ES module." TypeError before
   evaluation)

2. Check for node:process redirect

3. Resolve module (same cache + lookup path as static import)

4. Evaluate the module:
   a. If status == kErrored -> throw the stored exception
   b. If ESM and status == kEvaluating -> throw circular dependency error
   c. If status == kEvaluated or kEvaluating -> return namespace directly
      (allows CJS circular deps with incomplete exports, same as Node.js)
   d. Otherwise: call module.evaluate() -> Promise
      (wrapped in Lock::ModuleEvaluationScope, which tracks evaluation depth)
   e. If nested inside another module's evaluation (js.isEvaluatingModule()), do
      not drain microtasks -- doing so could trip a fatal V8 CHECK
      (status() >= kEvaluatingAsync). Otherwise (evaluation depth 0) run
      microtasks.
   f. Check promise state:
      - kFulfilled -> return module namespace
      - kRejected -> throw rejection reason
      - kPending -> throw "top-level await" error

5. Return the module namespace as v8::Object
```

## Evaluation and the Evaluator Pattern

Module evaluation has a key constraint: it must occur **outside** any
`IoContext`. The `Module::Evaluator` pattern enforces this.

### The EvalCallback

Set during registry construction via `Builder::setEvalCallback`. The standard
implementation (from `worker-modules.h`) creates a `SuppressIoContextScope`:

```cpp
builder.setEvalCallback([](Lock& js, const auto& module, auto v8Module,
                            const auto& observer) -> Promise<Value> {
  return js.tryOrReject<Value>([&] {
    SuppressIoContextScope suppressIoContextScope;
    KJ_DASSERT(!IoContext::hasCurrent());
    return check(v8Module->Evaluate(js.v8Context()));
  });
});
```

### How it flows

1. `module.evaluate(js, v8Module, observer, evaluator)` is called.
2. For ESM modules (`Flags::EVAL` always set):
   - The `evaluator(js, module, v8Module, observer)` is invoked.
   - The evaluator calls `ModuleRegistry::evaluateImpl` which invokes the
     `EvalCallback` (wrapping evaluation in `SuppressIoContextScope`).
   - The `EvalCallback` calls `v8Module->Evaluate()`. V8 evaluates the
     source text directly.
3. For Synthetic modules (dynamic import and require paths):
   - If `Flags::EVAL` is set, the evaluator is invoked first (same as ESM).
   - Otherwise, `module->Evaluate()` is called directly.
   - V8 sets status to `kEvaluating` and calls the `evaluationSteps` callback.
   - `evaluationSteps` calls `actuallyEvaluate()` which runs the callback
     to set exports.
   - V8 receives the result and sets status to `kEvaluated` or `kErrored`.
4. For static imports of synthetic modules, V8 drives `Module::Evaluate()`
   itself, which calls `evaluationSteps` → `actuallyEvaluate()`. The same
   code path executes; only the initial caller differs.

## `import.meta` Support

The `importMeta` callback is registered on the isolate during
`IsolateModuleRegistry` construction. When V8 creates an `import.meta` object:

1. Looks up the module in the `IsolateModuleRegistry` by v8::Module handle.
2. Sets three properties via `CreateDataProperty`:
   - `import.meta.main` — `true` if `Module::Flags::MAIN` is set
   - `import.meta.url` — the module's URL (e.g. `"file:///bundle/index.js"`)
   - `import.meta.resolve(specifier)` — resolves `specifier` relative to
     `import.meta.url` using WHATWG URL resolution (with `node:` bare-specifier
     handling when Node.js compat is enabled). Returns the resolved URL string,
     or throws a `TypeError` if the specifier cannot be resolved (matching
     Node.js and the HTML spec). Does **not** check if the resolved URL matches
     a module in the registry.

## The `ModuleNamespace` Helper

`Module::ModuleNamespace` wraps a `v8::Local<v8::Module>` and provides a safe
interface for synthetic module evaluation callbacks:

```cpp
class ModuleNamespace {
  bool set(Lock& js, kj::StringPtr name, JsValue value);    // Set a named export
  bool setDefault(Lock& js, JsValue value);                  // Set the "default" export
  kj::ArrayPtr<const kj::StringPtr> getNamedExports();       // Declared named exports
};
```

Internally calls `v8::Module::SetSyntheticModuleExport`. Returns `false` if the
export cannot be set (export name not declared, or V8 error). Validates that
`name` is in the declared exports set.

## Common Synthetic Module Handlers

Static factory methods on `Module` create `EvaluateCallback` closures for common
module types:

| Handler                | Created by                     | Export behavior                                  |
| ---------------------- | ------------------------------ | ------------------------------------------------ |
| `newTextModuleHandler` | `Module::newTextModuleHandler` | `default` = JS string from char data             |
| `newDataModuleHandler` | `Module::newDataModuleHandler` | `default` = ArrayBuffer (copied from byte data)  |
| `newJsonModuleHandler` | `Module::newJsonModuleHandler` | `default` = `JSON.parse(data)`                   |
| `newWasmModuleHandler` | `Module::newWasmModuleHandler` | `default` = `WebAssembly.Module` (compiled Wasm) |

The Wasm handler has its own cross-isolate cache: a `MutexGuarded<Maybe<CompiledWasmModule>>`
that stores the compiled module for reuse across isolates (similar to the ESM
compile cache).

### CJS-Style Module Handler

`Module::newCjsStyleModuleHandler<T, TypeWrapper>` is a template that creates a
handler for CommonJS-style modules:

1. Allocates a JSG resource object of type `T` (e.g. `CommonJsModuleContext`).
2. Compiles the source as a function via `ScriptCompiler::CompileFunction` with
   the JSG object as extension object (providing `module`, `exports`, `require`).
   The module's canonical URL is used as the compiled script's origin name: V8
   reports the origin as the referrer for dynamic `import()` performed by the
   script, which is how the dynamic import callback identifies the referring
   module. This also makes CJS stack-trace filenames consistent with ESM ones.
3. Calls the compiled function.
4. Extracts `exports` from the JSG object and sets them on the module namespace.

Re-evaluation is prevented by V8's module status machine (`kEvaluated` prevents
re-entry), not by application-level guards.

## Specifier Processing

### Bundle Module Names

Worker bundle module names (from config) are processed through `processModuleName`:

1. **Normalize**: resolve relative to `bundleBase`, remove `.`/`..`, normalize
   percent-encoding, strip query/fragment.
2. **Validate URL structure**: must be subordinate to `bundleBase` path.
3. **Protocol restrictions**: `cloudflare:`, `workerd:`, and `data:` protocols
   are forbidden in bundle module names (reserved for runtime use).

Result: a `file:///bundle/<path>` URL for file-protocol modules, or a fully
qualified URL for other protocols.

### Builtin Module Specifiers

Builtin modules use non-`file:` protocols (enforced by `ensureIsNotBundleSpecifier`):

- `node:buffer`, `node:crypto`, etc.
- `cloudflare:sockets`, `cloudflare:workers`, etc.
- `node-internal:*` (internal-only modules)
- `workerd:*` (internal-only modules)

These are parsed as absolute URLs where the protocol is the scheme and the
pathname is the module identifier.

### Node.js Compatibility Handling

When Node.js compat is enabled (`isNodeJsCompatEnabled(js)`):

- Bare specifiers like `"buffer"` are prefixed to `"node:buffer"` via `checkNodeSpecifier`.
- `"node:process"` is redirected to either `"node-internal:public_process"` or
  `"node-internal:legacy_process"` based on the `enable_nodejs_process_v2` flag.

## IsolateModuleRegistry — Per-Isolate Cache

The `IsolateModuleRegistry` binds the shared `ModuleRegistry` to a specific
V8 isolate. It is stored in context embedder data at
`ContextPointerSlot::MODULE_REGISTRY` and retrieved via
`IsolateModuleRegistry::from(isolate)`.

### Two-Level Cache

The per-isolate cache is split into two structures (see the architecture
diagram above for the identity rules this encodes):

| Structure        | Shape                                                     | Used by                                        |
| ---------------- | --------------------------------------------------------- | ---------------------------------------------- |
| `instantiations` | `kj::Table<Entry>` indexed by v8 handle and by (URL, def) | Reverse lookup (importMeta, resolve callbacks) |
| `resolutions`    | `HashMap<SpecifierContext, const Module*>`                | Primary resolution and referrer probing        |

Lookup properties:

- **Resolving by specifier** is two O(1) hash lookups: (type, URL) → definition,
  then (URL, definition) → Entry.
- **Reverse lookup** (v8::Module to Entry) is O(1) hash lookup (vs legacy's O(n)
  linear scan over all entries).
- **Referrer lookup** probes the resolutions map once per context type in
  resolution-priority order (at most four O(1) lookups).

### Cache Population

On first resolution of a specifier:

1. `resolveWithCaching` strips query params and fragments from the specifier.
2. Calls `inner.lookup()` on the shared `ModuleRegistry` (acquires exclusive lock).
3. If found, records (type, URL) → definition in `resolutions`.
4. Reuses the existing instantiation for (URL, definition) if one exists;
   otherwise calls `module.getDescriptor(js, observer)` to create the
   v8::Module and inserts a new `Entry`.
5. Subsequent lookups for the same specifier hit the cache directly.

## V8 Callback Registration

During `IsolateModuleRegistry` construction, three callbacks are registered:

```cpp
setAlignedPointerInEmbedderData(context, MODULE_REGISTRY, this);
isolate->SetHostImportModuleDynamicallyCallback(&dynamicImport);
isolate->SetHostImportModuleWithPhaseDynamicallyCallback(&dynamicImportWithPhase);
isolate->SetHostInitializeImportMetaObjectCallback(&importMeta);
```

The `resolveModuleCallback<false>` and `resolveModuleCallback<true>` are passed
to `InstantiateModule` during each module instantiation (not set globally on the
isolate).

### Source Phase Imports

Source phase imports (`import source x from "mod"`) are handled through:

- `resolveModuleCallback<true>` — returns `v8::Object` (not `v8::Module`)
- `dynamicImportWithPhase` — wraps `dynamicImportModuleCallback` with `SourcePhase::YES`

Only WebAssembly modules support source phase imports. The resolved Wasm
module is evaluated, and the `WebAssembly.Module` object (the compiled Wasm,
not an instance) is extracted from the module's `default` export and returned.
All other module types throw `SyntaxError`.

## Registry Setup and Population

### 1. Construction (`newWorkerModuleRegistry<TypeWrapper>` in `worker-modules.h`)

The main entry point. Called during `Worker::Script` construction:

```
newWorkerModuleRegistry<TypeWrapper>(resolveObserver, maybeSource, flags, bundleBase, setupForApi)
  |
  |-- Create ModuleRegistry::Builder
  |
  |-- Set EvalCallback (SuppressIoContextScope wrapper)
  |
  |-- registerBuiltinModules<TypeWrapper>(builder, featureFlags)
  |     |-- Node.js compat modules (internal + external bundles)
  |     |-- Sockets, Base64, MessageChannel, RPC modules
  |     |-- Unsafe modules (if enabled)
  |     |-- RTTI module (if enabled)
  |     |-- Cloudflare bundle (from CLOUDFLARE_BUNDLE capnp)
  |     |-- Workers module, Tracing module
  |     +-- Internal-only: EnvModule, FileSystemModule
  |
  |-- Register worker bundle modules (if any)
  |     |-- Load capnp schemas into SchemaLoader
  |     |-- Create BundleBuilder(bundleBase)
  |     |-- For each module in source:
  |     |     |-- EsModule -> addEsmModule (the configured mainModule gets the
  |     |     |     MAIN flag; entry points must be ESM, enforced at resolution
  |     |     |     via RequireOption::REQUIRE_ESM)
  |     |     |-- TextModule -> addSyntheticModule(newTextModuleHandler)
  |     |     |-- DataModule -> addSyntheticModule(newDataModuleHandler)
  |     |     |-- WasmModule -> addWasmModule
  |     |     |-- JsonModule -> addSyntheticModule(newJsonModuleHandler)
  |     |     |-- CommonJsModule -> addSyntheticModule(newCjsStyleModuleHandler)
  |     |     +-- CapnpModule -> addSyntheticModule(capnp handler with lazy export)
  |     +-- builder.add(bundleBuilder.finish())
  |
  |-- setupForApi(builder, isPythonWorker)   // API-specific extensions
  |
  +-- builder.finish() -> kj::Arc<ModuleRegistry>
```

### 2. Attachment to Isolate (`resource.h:1913-1922`)

During V8 context creation (`JsContext` construction):

```cpp
if (options.newModuleRegistry) {
  return newModuleRegistry.attachToIsolate(js, compilationObserver);
} else {
  return ModuleRegistryImpl<TypeWrapper>::install(isolate, context, compilationObserver);
}
```

`attachToIsolate` creates an `IsolateModuleRegistry`, stores it in embedder data,
and registers V8 callbacks. Returns `kj::Own<void>` whose destructor cleans up.

### 3. Dispatch Between Legacy and New (`jsg.c++`)

`Lock::resolveModule` and `Lock::resolveInternalModule` check
`IsolateBase::isUsingNewModuleRegistry()` and dispatch accordingly:

```cpp
if (isolate.isUsingNewModuleRegistry()) {
  return modules::ModuleRegistry::tryResolveModuleNamespace(js, specifier, ...);
} else {
  // legacy ModuleRegistry path
}
```

The isolate-level bit is set at isolate construction from
`isNewModuleRegistryEnabled(flags)` (`io/features.h`), which returns false for
Python workers regardless of the `new_module_registry` flag. All other
flag-check sites (`server.c++` registry creation, `worker.c++` compile paths and
the nodejs_compat_v2 process/buffer warm-up, and the api-level require paths)
route through the same function, so a worker can never be split across the two
registries. Note that `Cloudflare.compatibilityFlags.new_module_registry` as
observed by user code still reflects the configured flag value; the function
describes the resulting behavior.

## Resolution Priority and Isolation Rules

### Referrer-Based Context Determination

When a V8 resolve callback fires, the referrer module's type determines the
`ResolveContext::Type`:

| Referrer Module Type | Resolve Context Type | Can see                   |
| -------------------- | -------------------- | ------------------------- |
| `BUNDLE`             | `BUNDLE`             | Bundle, Builtin, Fallback |
| `BUILTIN`            | `BUILTIN`            | Builtin, BuiltinOnly      |
| `BUILTIN_ONLY`       | `BUILTIN_ONLY`       | BuiltinOnly only          |
| `FALLBACK`           | `BUNDLE`             | Bundle, Builtin, Fallback |

This means:

- User code **can** import builtins (e.g. `import { Buffer } from 'node:buffer'`).
- User code **cannot** import internal-only modules.
- Builtin code **can** import internal-only modules.
- Builtin code **cannot** import user bundle modules.
- User bundle modules with the same specifier as a builtin **shadow** the builtin
  (Bundle is searched before Builtin in BUNDLE resolution).

### Alias Handling

Aliases are redirects. When a bundle lookup returns a string (redirect)
instead of a `Module`:

1. The string is parsed as a URL (relative to `bundleBase` if needed).
2. `lookupImpl` recurses with the new specifier. Chains of aliases are
   followed to any depth; each resolution tracks the specifiers it has
   visited, and a redirect to an already-visited specifier (a cycle) resolves
   to "module not found".

### Error Propagation for Errored Dependencies

V8's `InnerModuleEvaluation` only recurses into `SourceTextModule` dependencies
when checking for `kErrored` status — it skips `SyntheticModule` dependencies
entirely. This means if a synthetic module is `kErrored`, an ESM that imports
it would evaluate successfully with `undefined` export values instead of
propagating the error.

To work around this, `resolveModuleCallback` checks the resolved module's
status after resolution. If the module is `kErrored`, the callback throws the
module's cached exception instead of returning the handle to V8. This prevents
V8 from instantiating an ESM graph containing errored synthetic dependencies.

Similarly, `dynamicResolve` checks for `kErrored` before calling `evaluate()`,
returning a rejected promise with the cached exception.

## Thread Safety Model

### Shared State (ModuleRegistry, ModuleBundle, Module)

- `ModuleRegistry::lookup()` acquires `impl.lockExclusive()` for every call.
  This serializes all resolution across all isolate replicas.
- `ModuleBundle` and `Module` objects are immutable after construction, with
  two exceptions:
  - `EsModule::cachedData` uses `kj::MutexGuarded` (shared lock for reads,
    exclusive for writes).
  - `newWasmModuleHandler` similarly uses `kj::MutexGuarded` for its compiled
    Wasm cache.
  - `StaticModuleBundle::cache` and `FallbackModuleBundle::storage/aliases` are
    mutated during lookup, but are protected by the registry's exclusive lock.

### Per-Isolate State (IsolateModuleRegistry)

- `IsolateModuleRegistry` is created and used only within a single V8 isolate.
- The `lookupCache` is not thread-safe on its own; safety comes from V8's
  single-threaded execution model (only one thread holds the isolate lock).
- V8 callbacks (`resolveModuleCallback`, `dynamicImport`, `importMeta`) always
  run while holding the isolate lock.

## Key Characteristics

1. **Fully lazy.** No module is compiled until first import. No module is
   evaluated until first use. Unreferenced modules cost nothing.

2. **URL-based specifiers.** All module identity and resolution uses WHATWG URLs.
   Relative resolution uses standard URL semantics via ada-url.

3. **O(1) lookups.** Both forward (specifier to module) and reverse
   (v8::Module to definition) lookups are hash-indexed. The legacy registry's
   O(n) reverse scan is eliminated.

4. **Cross-isolate caching.** ESM compile cache and Wasm compiled modules are
   shared across isolate replicas, reducing startup time for subsequent replicas.

5. **Compile cache is transparent.** V8 bytecode caching is handled entirely
   within `EsModule::getDescriptor()`. Callers never see compile cache details.

6. **Evaluation callbacks are idempotent.** Synthetic module `EvaluateCallback`
   functions can be called multiple times from multiple isolates. They create
   fresh JS objects each time.

7. **Import attributes are validated.** The `type` import attribute is supported
   for both static and dynamic imports. Each `Module` has a `ContentType`
   (`NONE`, `JSON`, `TEXT`, `DATA`, `WASM`) set at construction time. Supported
   type values and their corresponding TC39 proposals:
   - `type: 'json'` — JSON Modules (TC39 Stage 4, finished) → `ContentType::JSON`.
     This is the only type currently enabled.
   - `type: 'text'` — Import Text (TC39 Stage 3) → `ContentType::TEXT`.
     Recognized but rejected with "not yet supported" pending Stage 4.
   - `type: 'bytes'` — Import Bytes (TC39 Stage 2.7) → `ContentType::DATA`.
     Recognized but rejected with "not yet supported". The proposal requires
     `Uint8Array` backed by an immutable `ArrayBuffer`, which is not yet
     implemented. Data modules currently expose mutable `ArrayBuffer`s.
     All three types are parsed in `parseImportAttributes`. Only `json` passes
     validation in `validateImportType`; `text` and `bytes` are rejected there
     with specific error messages. The `ContentType` enum and module tagging are
     fully plumbed for all three, ready to enable when the proposals are ready.
     When a type attribute is specified, the resolved module's content type must
     match or a `TypeError` is thrown. Unrecognized attribute keys (anything
     other than `type`) and unsupported type values are rejected with `TypeError`.
     Attribute parsing is handled
     by `parseImportAttributes()` which reads V8's `FixedArray` of key-value-
     location triples. Validation is handled by `validateImportType()` which
     compares the declared type against `Module::contentType()`. Both static
     imports (`resolveModuleCallback`) and dynamic imports
     (`dynamicImportModuleCallback` → `dynamicResolve`) use these helpers.

8. **`require()` return value semantics (UNWRAP_DEFAULT).** When callers use the
   `UNWRAP_DEFAULT` require option (used by `createRequire` and
   `CommonJsModuleContext::require`), the checks run in this order:
   - **Any ESM with a truthy `__cjsUnwrapDefault` export** (an ecosystem bundler
     convention that predates Node's require(esm) support, checked first to
     match the legacy registry's order): returns the `default` export.
   - **Any ESM defining the string-named export `'module.exports'`** (Node.js'
     official mechanism for an ES module to control its require() result):
     returns that export's value.
   - **User ESM (worker bundle and fallback modules)**: returns the module
     namespace, matching Node.js `require(esm)` behavior.
   - **Builtin ESM** (`node:assert`, `node:buffer`, etc.): returns the `default`
     export, because workerd implements builtins as ESM that wrap CJS-style APIs
     in a default export. Falls back to the namespace if a builtin defines no
     default export — require() never yields `undefined` for a resolvable module.
   - **Synthetic modules** (CJS, JSON, Text, Data, WASM): returns the `default`
     export, which is where the module's value lives (`module.exports` for CJS,
     parsed value for JSON, etc.).
     Without `UNWRAP_DEFAULT`, `require()` always returns the full module namespace.
     The `export_commonjs_default` and `require_returns_default_export` compat
     flags describe legacy-registry behavior only and are not consulted.

9. **Python workers do not use the new registry.** When `python_workers` is
   enabled, the `new_module_registry` flag is ignored entirely:
   `isNewModuleRegistryEnabled()` (in `io/features.h`) returns false and the
   worker uses the legacy registry. Every decision about which registry to use
   must go through that function rather than reading the flag directly. The
   `KJ_FAIL_ASSERT` for `PythonModule` content in `worker-modules.h` remains as
   defense-in-depth.

10. **Top-level await handling.** After `Evaluate`, microtasks are drained unless
    the evaluation is nested inside another module's evaluation (evaluation depth
    tracked by `Lock::ModuleEvaluationScope`, queried via
    `js.isEvaluatingModule()`). Draining while an ancestor module is still
    `kEvaluating` could fire an async module's fulfillment callback early and trip
    a fatal V8 CHECK (`status() >= kEvaluatingAsync`). If the returned Promise is
    still pending, a "top-level await" error is thrown. Workers must fully
    initialize synchronously.

    The legacy registry (`modules.c++`) follows the same rules.
