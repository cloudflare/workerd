# Legacy Module Registry (`jsg::ModuleRegistry` / `jsg::ModuleRegistryImpl`)

Reference for reasoning about the original (legacy) module registry implementation,
how it interfaces with V8's module APIs, and how modules flow from config through
compilation to evaluation. For V8 module internals, see
[v8-module-internals.md](v8-module-internals.md). For the new replacement
implementation, see [new-module-registry.md](new-module-registry.md).

## Source Files

| File                                   | Role                                                                                                                                       |
| -------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------ |
| `src/workerd/jsg/modules.h`            | `ModuleRegistry` abstract base, `ModuleRegistryImpl<TypeWrapper>` full implementation, V8 callback templates, `ModuleInfo` / `Entry` types |
| `src/workerd/jsg/modules.c++`          | V8 resolve callback, synthetic module evaluation callback, `instantiateModule`, `compileEsmModule`, `createSyntheticModule`, `requireImpl` |
| `src/workerd/jsg/modules.capnp`        | Cap'n Proto schema: `Bundle`, `Module`, `ModuleType` enum                                                                                  |
| `src/workerd/jsg/resource.h:1913-1922` | Registry installation into V8 context (picks legacy vs new)                                                                                |
| `src/workerd/jsg/jsg.c++:378-395`      | `Lock::resolveModule` dispatches between legacy and new registry                                                                           |
| `src/workerd/io/worker-modules.h`      | `newWorkerModuleRegistry` (new registry), `modules::legacy::*` helpers, Python module registration                                         |
| `src/workerd/io/worker-modules.c++`    | Python metadata state construction                                                                                                         |
| `src/workerd/server/workerd-api.c++`   | `WorkerdApi::compileModules` (legacy path), `WorkerdApi::newWorkerdModuleRegistry` (new path), extension loading                           |
| `src/workerd/api/modules.h`            | `registerModules` — registers all built-in API modules (Node.js compat, sockets, crypto, cloudflare bundle, etc.)                          |

## Architecture Overview

```
ModuleRegistryImpl<TypeWrapper>           (per-isolate, stored in V8 context embedder data)
  ├── entries: kj::Table<Own<Entry>>      (hash-indexed by (specifier, type))
  │     Each Entry is one of:
  │       ├── ModuleInfo                  (already-compiled v8::Module + optional synthetic data)
  │       ├── kj::ArrayPtr<const char>    (uncompiled source — compiled lazily on first resolve)
  │       └── ModuleCallback              (factory function — called lazily on first resolve)
  │
  ├── dynamicImportHandler                (embedder callback for dynamic import context setup)
  └── fallbackServiceRedirects            (cache for fallback service redirects)
```

The registry is installed into the V8 context's embedder data at slot
`ContextPointerSlot::MODULE_REGISTRY`. It is retrieved via
`ModuleRegistry::from(js)` which reads the aligned pointer from embedder data.

## Module Types (`ModuleType` enum, `modules.capnp`)

| Type       | Value | Purpose                                                                | Resolution priority                       |
| ---------- | ----- | ---------------------------------------------------------------------- | ----------------------------------------- |
| `BUNDLE`   | 0     | User worker modules                                                    | Searched first in DEFAULT resolution      |
| `BUILTIN`  | 1     | Runtime-provided modules importable by user code (e.g., `node:buffer`) | Searched after BUNDLE                     |
| `INTERNAL` | 2     | Runtime-internal modules only importable by other builtins             | Searched only in INTERNAL_ONLY resolution |

## The Entry Table

The core data structure is a `kj::Table` with a `kj::HashIndex`:

```cpp
kj::Table<kj::Own<Entry>, kj::HashIndex<SpecifierHashCallbacks>> entries;
```

### Entry Key

The hash key is `(kj::Path specifier, ModuleType type)`. This means the **same
specifier can exist multiple times** in the table with different types. A BUNDLE
module and a BUILTIN module with the same path are distinct entries.

```cpp
struct Key {
  const kj::Path& specifier;
  const Type type;
  uint hash;  // kj::hashCode(specifier, type)
};
```

### Entry Info — Lazy Compilation

Each `Entry` holds a `kj::OneOf<ModuleInfo, kj::ArrayPtr<const char>, ModuleCallback>`:

| Variant                    | Meaning                                                                     | When                                                                                                |
| -------------------------- | --------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------- |
| `ModuleInfo`               | Already compiled — ready to use                                             | Worker bundle modules (compiled eagerly at startup), or after first lazy resolve                    |
| `kj::ArrayPtr<const char>` | Raw source code, not yet compiled                                           | Builtin ESM modules registered via `addBuiltinModule(specifier, sourceCode, type)`                  |
| `ModuleCallback`           | Factory function `(Lock&, ResolveMethod, Maybe<Path&>) → Maybe<ModuleInfo>` | Builtin modules that need runtime-dependent construction (Wasm, data, JSON, object-wrapper modules) |

The `Entry::module()` method implements lazy compilation:

```cpp
kj::Maybe<ModuleInfo&> module(jsg::Lock& js, CompilationObserver& observer, ...) {
  KJ_SWITCH_ONEOF(info) {
    KJ_CASE_ONEOF(moduleInfo, ModuleInfo) { return moduleInfo; }
    KJ_CASE_ONEOF(src, kj::ArrayPtr<const char>) {
      info = ModuleInfo(js, specifier.toString(), src, compileCache,
          ModuleInfoCompileOption::BUILTIN, observer);
      return info.tryGet<ModuleInfo>();
    }
    KJ_CASE_ONEOF(factory, ModuleCallback) {
      KJ_IF_SOME(result, factory(js, method, referrer)) { info = kj::mv(result); }
      return info.tryGet<ModuleInfo>();
    }
  }
}
```

After lazy compilation, the `info` field is **mutated in place** from source/callback
to `ModuleInfo`. Subsequent resolves return the cached `ModuleInfo` directly.

## ModuleInfo — The Compiled Module Record

```cpp
struct ModuleInfo {
  HashableV8Ref<v8::Module> module;   // The V8 module handle (ESM or Synthetic)
  kj::Maybe<SyntheticModuleInfo> maybeSynthetic;  // Payload for synthetic modules
  kj::Maybe<kj::Array<kj::String>> maybeNamedExports;  // CJS named exports
  kj::Maybe<V8Ref<v8::Object>> maybeModuleSourceObject;  // Source phase import object
  mutable kj::Maybe<V8Ref<v8::Object>> maybeMutableExports;  // Cached mutable require() wrapper
};
```

### SyntheticModuleInfo Variants

```cpp
using SyntheticModuleInfo = kj::OneOf<
    CapnpModuleInfo,     // Cap'n Proto schema module (default + named exports)
    CommonJsModuleInfo,  // CommonJS module (eval func + provider)
    DataModuleInfo,      // Binary data (ArrayBuffer)
    TextModuleInfo,      // Text content (String)
    WasmModuleInfo,      // WebAssembly module (WasmModuleObject)
    JsonModuleInfo,      // JSON value
    ObjectModuleInfo     // Wrapped JSG object
>;
```

When `maybeSynthetic` is `kj::none`, the module is a real ESM (compiled from
JavaScript source via `ScriptCompiler::CompileModule`).

When `maybeSynthetic` has a value, the `module` field holds a V8 `SyntheticModule`
created via `v8::Module::CreateSyntheticModule`, and the variant holds the data
needed to populate exports during evaluation.

### How ModuleInfo is Constructed

Three constructors:

1. **ESM from source**: `ModuleInfo(js, name, content, compileCache, flags, observer)`
   → calls `compileEsmModule()` → `ScriptCompiler::CompileModule`

2. **From pre-compiled v8::Module**: `ModuleInfo(js, module, maybeSynthetic)`
   → wraps an existing V8 module handle

3. **Synthetic module**: `ModuleInfo(js, name, maybeExports, synthetic)`
   → calls `createSyntheticModule()` → `v8::Module::CreateSyntheticModule`
   with `evaluateSyntheticModuleCallback` as the evaluation steps

## Resolution Flow

### `resolve(js, specifier, referrer, option, method, rawSpecifier)`

The primary resolution method. Returns `kj::Maybe<ModuleInfo&>`.

```
resolve(specifier, option=DEFAULT)
  ├── option == INTERNAL_ONLY?
  │     └── Look up entries[(specifier, INTERNAL)]
  ├── option == BUILTIN_ONLY?
  │     └── Look up entries[(specifier, BUILTIN)]
  └── option == DEFAULT?
        ├── Look up entries[(specifier, BUNDLE)]     ← user modules first
        ├── Look up entries[(specifier, BUILTIN)]    ← then builtins
        └── Try fallback service (if configured)
              ├── Check redirect cache
              ├── Call tryResolveFromFallbackService()
              └── Insert result into entries, resolve again
```

### `resolve(js, v8::Local<v8::Module>)` — Reverse Lookup

Given a V8 module handle, finds the corresponding `ModuleRef`. This is needed by
the V8 resolve callback to identify the referrer module.

**This is a linear scan** over all entries (O(n)):

```cpp
for (const kj::Own<Entry>& entry: entries) {
  KJ_IF_SOME(info, entry->info.tryGet<ModuleInfo>()) {
    if (info.module == module) { return ModuleRef{...}; }
  }
}
```

This is explicitly noted as slow in the code comments. It cannot use hash lookup
because entries may be lazily compiled (the ModuleInfo might not exist yet for some
entries, and the v8::Module handle only exists after compilation).

## V8 Callback Integration

### Resolve Callback — `resolveModuleCallback<IsSourcePhase>`

Registered via `module->InstantiateModule(context, resolveModuleCallback<false>,
resolveModuleCallback<true>)`. Called by V8 during `PrepareInstantiate` for each
`import` statement.

Template parameter `IsSourcePhase`:

- `false`: evaluation phase import — returns `v8::MaybeLocal<v8::Module>`
- `true`: source phase import (`import source`) — returns `v8::MaybeLocal<v8::Object>`

Flow:

1. Get the `ModuleRegistry` from context embedder data.
2. **Reverse-lookup the referrer** via `registry->resolve(js, referrer)` (linear scan).
3. Determine the referrer's type (BUNDLE vs BUILTIN/INTERNAL).
4. Parse the specifier, applying `node:` prefix normalization if Node.js compat is enabled.
5. Resolve against the referrer path: `ref.specifier.parent().eval(spec)`.
   (Known prefixes `node:`, `cloudflare:`, `workerd:` bypass parent-relative resolution.)
6. Call `registry->resolve(js, targetPath, ...)` with:
   - `INTERNAL_ONLY` if the referrer is a builtin/internal module
   - `DEFAULT` otherwise
7. For evaluation phase: return `resolved.module.getHandle(js)`.
8. For source phase: return `resolved.getModuleSourceObject(js)` or throw SyntaxError.
9. On not-found: retry with absolute path for `node:`/`cloudflare:` prefixed specifiers.
10. Final fallback: throw `"No such module"` error.

Errors are handled via `js.tryCatch`: exceptions are caught, and
`js.v8Isolate->ThrowException()` is called to schedule them on the isolate
(V8 requires exceptions to be scheduled, not thrown as C++ exceptions, during
resolve callbacks).

### Synthetic Module Evaluation Callback — `evaluateSyntheticModuleCallback`

A single C++ function registered as the `SyntheticModuleEvaluationSteps` for **all**
synthetic modules. Called by V8 during `SyntheticModule::Evaluate`.

Flow:

1. Get the `ModuleRegistry` from context embedder data.
2. **Reverse-lookup the module** via `registry->resolve(js, module)` (linear scan).
3. Extract `ref.module.maybeSynthetic` — must be non-none.
4. Switch on the synthetic variant type:

| Variant              | Export Behavior                                                                                                                                                                     |
| -------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `CapnpModuleInfo`    | Sets `default` to `fileScope`, then each named export from `topLevelDecls`                                                                                                          |
| `CommonJsModuleInfo` | Calls `evalFunc()` (runs the CJS body), then sets `default` to the `exports` object. If `maybeNamedExports` is set, also sets each named export by reading from the exports object. |
| `TextModuleInfo`     | Sets `default` to the text string                                                                                                                                                   |
| `DataModuleInfo`     | Sets `default` to the ArrayBuffer                                                                                                                                                   |
| `WasmModuleInfo`     | Sets `default` to the WasmModuleObject                                                                                                                                              |
| `JsonModuleInfo`     | Sets `default` to the parsed JSON value                                                                                                                                             |
| `ObjectModuleInfo`   | Sets `default` to the wrapped C++ object                                                                                                                                            |

All non-CJS variants follow the same pattern:

```cpp
module->SetSyntheticModuleExport(js.v8Isolate, defaultStr, value)
```

5. On success: return a resolved Promise (V8 requires this).
6. On error: schedule the exception on the isolate and return empty MaybeLocal.

### Dynamic Import Callback — `dynamicImportCallback<TypeWrapper>`

Registered on the isolate via `isolate->SetHostImportModuleDynamicallyCallback`.

Flow:

1. Check import attributes — reject if unrecognized and the compat flag is set.
2. Parse the referrer's resource name into a `kj::Path`.
3. Apply `node:` prefix normalization.
4. Special-case `node:process` → redirect to internal module based on compat flag.
5. Resolve the specifier against the referrer path.
6. Call `registry->resolveDynamicImport(js, specifierPath, referrerPath, rawSpec)`.
7. The `resolveDynamicImport` method:
   - Determines the referrer's type (BUNDLE → DEFAULT resolution, BUILTIN → INTERNAL_ONLY).
   - Resolves the module.
   - If a `dynamicImportHandler` is set: wraps the instantiation in the handler
     (used to set up `SuppressIoContextScope`).
   - Calls `instantiateModule(js, module)` and returns `module->GetModuleNamespace()`.
8. Returns a Promise that resolves to the module namespace.

## Module Instantiation — `instantiateModule`

```cpp
void instantiateModule(jsg::Lock& js, v8::Local<v8::Module>& module,
    InstantiateModuleOptions options);
```

This is workerd's wrapper around V8's `InstantiateModule` + `Evaluate`:

1. If status is `kErrored`: throw the module's exception immediately.
2. If status is `kEvaluated` or `kEvaluating`: return (already done).
3. If status is `kUninstantiated`: call `module->InstantiateModule(context,
resolveModuleCallback<false>, resolveModuleCallback<true>)`.
   This triggers the V8 two-phase instantiation, calling back into
   `resolveModuleCallback` for each dependency.
4. Call `module->Evaluate(context)` — returns a Promise.
5. If the module graph is async and the promise is pending:
   - If `NO_TOP_LEVEL_AWAIT` option: throw error.
6. Run `js.runMicrotasks()` to attempt settling pending promises.
7. Check the promise state:
   - `kPending`: throw `"Top-level await in module is unsettled."`
   - `kRejected`: throw the module's exception.
   - `kFulfilled`: success.

### `onlyInstantiateModule` — Link Without Evaluating

```cpp
void onlyInstantiateModule(jsg::Lock& js, v8::Local<v8::Module>& module);
```

Calls `InstantiateModule` but **not** `Evaluate`. Used when the caller only
needs the module linked but will defer evaluation.

## CommonJS Module Support

CommonJS modules are implemented as synthetic modules with special evaluation:

### `CommonJsModuleInfo`

```cpp
struct CommonJsModuleInfo {
  kj::Own<CommonJsModuleProvider> provider;  // getContext(), getExports()
  jsg::Function<void()> evalFunc;            // compiled wrapper function
};
```

The CJS module body is compiled as a function (not a module) via
`v8::ScriptCompiler::CompileFunction`, with the CJS context object
(`module`, `exports`, `require`, etc.) as the extension object.

During synthetic module evaluation:

1. `evalFunc()` is called — this runs the CJS body, populating `module.exports`.
2. `commonjs.getExports(js)` retrieves the final exports.
3. The result is set as the synthetic module's `default` export.
4. If `maybeNamedExports` is present, each named property is also set as an
   individual export on the synthetic module.

### Circular Dependency Handling

In `requireImpl`, if the module status is `kEvaluating` or `kInstantiating`
(indicating a circular dependency), the CJS provider's current exports are
returned directly — matching Node.js behavior for circular CJS requires.

## `require()` Implementation — `requireImpl`

`ModuleRegistry::requireImpl(js, info, options)` implements the `require()`
function used by CJS modules and Node.js compat:

1. Get the v8::Module handle from `info.module`.
2. Check for circular dependency (kEvaluating/kInstantiating with CJS → return
   current exports).
3. Check top-level-await compat flag.
4. Call `instantiateModule(js, module, opts)`.
5. Apply `require(esm)` conventions:
   - If ESM and `__cjsUnwrapDefault` is true → return default export.
   - If ESM and `module.exports` key exists → return that (Node.js `require(esm)` convention).
   - If `EXPORT_DEFAULT` option → return `namespace.default`.
   - If `require_returns_default_export` flag → return default export or mutable copy.
   - Otherwise → return the full module namespace.

## Registry Setup and Population

### 1. Installation (`resource.h:1913-1922`)

During V8 context creation (`JsContext` construction):

```cpp
// Legacy path:
ModuleRegistryImpl<TypeWrapper>::install(isolate, context, compilationObserver);
// This allocates the registry on the heap, stores a raw pointer in context
// embedder data, and sets the dynamic import callback on the isolate.
```

### 2. Worker Bundle Compilation (`WorkerdApi::compileModules`)

Called during `Worker::Script` construction. For each module in the worker bundle:

1. Reads the module definition from the Cap'n Proto config.
2. Calls `tryCompileLegacyModule` which dispatches by content type:
   - `EsModule` → `ModuleInfo(js, name, content, compileCache, BUNDLE, observer)`
   - `TextModule` → `ModuleInfo(js, name, none, TextModuleInfo(js, string))`
   - `DataModule` → `ModuleInfo(js, name, none, DataModuleInfo(js, arrayBuffer))`
   - `WasmModule` → `ModuleInfo(js, name, none, WasmModuleInfo(js, wasmModule))`
   - `JsonModule` → `ModuleInfo(js, name, none, JsonModuleInfo(js, parsedValue))`
   - `CommonJsModule` → `ModuleInfo(js, name, namedExports, CommonJsModuleInfo(...))`
   - `CapnpModule` → `addCapnpModule<JsgIsolate>(lock, typeId, name)`
3. Inserts each `ModuleInfo` into the registry as `Type::BUNDLE`.

### 3. Built-in Module Registration (`api::registerModules`)

Called after bundle compilation:

```cpp
void registerModules(Registry& registry, auto featureFlags) {
  node::registerNodeJsCompatModules(registry, featureFlags);
  registerUnsafeModules(registry, featureFlags);
  registerSocketsModule(registry, featureFlags);
  registerBase64Module(registry, featureFlags);
  registerMessageChannelModule(registry, featureFlags);
  registry.addBuiltinBundle(CLOUDFLARE_BUNDLE);
  registerWorkersModule(registry, featureFlags);
  registerTracingModule(registry, featureFlags);
  // ... internal modules ...
}
```

Each `addBuiltinModule` call creates an `Entry` with type BUILTIN or INTERNAL.
ESM builtins are registered as raw source (`kj::ArrayPtr<const char>`) for lazy
compilation. Object-wrapper builtins use `ModuleCallback` factories.

### 4. Extensions (`workerd-api.c++:563-568`)

Server extensions (loaded from config) are registered as BUILTIN or INTERNAL:

```cpp
for (auto extension: impl->extensions) {
  for (auto module: extension.getModules()) {
    modules->addBuiltinModule(module.getName(), module.getEsModule().asArray(),
        module.getInternal() ? Type::INTERNAL : Type::BUILTIN);
  }
}
```

### 5. Python Modules

Python workers have special handling via `registerPythonWorkerdModules`, which:

- Replaces the main module with a JS shim (`PYTHON_ENTRYPOINT`).
- Registers pyodide packages and runtime infrastructure as INTERNAL modules.

## Resolution Priority and Isolation Rules

### By Resolve Option

| Option          | Search order                | Use case                         |
| --------------- | --------------------------- | -------------------------------- |
| `DEFAULT`       | BUNDLE → BUILTIN → fallback | User code imports                |
| `BUILTIN_ONLY`  | BUILTIN only                | Explicit builtin resolution      |
| `INTERNAL_ONLY` | INTERNAL only               | Builtin code importing internals |

### Referrer-Based Isolation

When the V8 resolve callback fires, the referrer's type determines isolation:

- **BUNDLE referrer** → `DEFAULT` resolution (can see BUNDLE + BUILTIN)
- **BUILTIN or INTERNAL referrer** → `INTERNAL_ONLY` resolution (can only see INTERNAL)

This means:

- User code **can** import builtins (e.g., `import { Buffer } from 'node:buffer'`).
- User code **cannot** import internal modules.
- Builtin code **can** import internal modules.
- Builtin code **cannot** import user bundle modules.
- User bundle modules with the same name as a builtin **shadow** the builtin
  (BUNDLE is searched before BUILTIN in DEFAULT resolution).

### Dynamic Import Isolation

`resolveDynamicImport` performs the same referrer-based check:

```cpp
if (entries.find(Key(referrer, Type::BUNDLE)) != kj::none) {
  resolveOption = DEFAULT;
} else if (entries.find(Key(referrer, Type::BUILTIN)) != kj::none) {
  resolveOption = INTERNAL_ONLY;
}
```

## Fallback Service

When resolution fails and a fallback service is configured
(`IsolateBase::tryGetModuleFallback`), the registry attempts to load the module
from an external service. This is used in local development (e.g., `wrangler dev`)
where not all modules may be present in the bundle.

Results from the fallback service are:

- `ModuleInfo`: inserted into the registry as BUNDLE (or BUILTIN if the specifier
  has a known prefix).
- `kj::String`: a redirect — stored in `fallbackServiceRedirects` for future lookups,
  then resolution retries with the redirected path.

## How V8 Modules Map to ModuleInfo

### ESM Modules (maybeSynthetic == kj::none)

```
v8::ScriptCompiler::CompileModule(isolate, &source)
  → v8::Module (SourceTextModule internally)
  → Stored in ModuleInfo.module
  → V8 handles all import/export resolution via resolveModuleCallback
  → Evaluation runs the module's JavaScript code
```

### Synthetic Modules (maybeSynthetic has a value)

```
v8::Module::CreateSyntheticModule(isolate, name, exportNames, evaluateSyntheticModuleCallback)
  → v8::Module (SyntheticModule internally)
  → Stored in ModuleInfo.module
  → V8 calls evaluateSyntheticModuleCallback during Evaluate
  → Callback looks up the ModuleInfo via reverse scan
  → Switches on SyntheticModuleInfo variant to set exports
```

All synthetic modules share the **same** evaluation callback function
(`evaluateSyntheticModuleCallback`). The callback uses the reverse module lookup to
find which `ModuleInfo` corresponds to the `v8::Module` being evaluated, then reads
the synthetic data from it.

### Export Names

For synthetic modules, export names are declared at creation time:

- All synthetic modules always have a `"default"` export.
- CJS modules with `maybeNamedExports` also declare those names.
- Cap'n Proto modules declare names for each nested schema node.

For ESM modules, export names are determined by V8 during compilation (extracted
from the source text).

## Key Characteristics and Limitations

1. **Per-isolate, not shared.** Each V8 isolate has its own `ModuleRegistryImpl`
   instance. Modules compiled in one isolate cannot be shared with another. The
   same source code is recompiled for each isolate.

2. **Eager compilation for bundle modules.** All worker bundle modules are compiled
   during `compileModules`, before any request is served. Builtin modules are lazily
   compiled on first import.

3. **Path-based specifiers using `kj::Path`.** All specifiers are parsed into
   `kj::Path` objects. Relative imports are resolved against the referrer's parent
   path. This means `./foo` from `bar/baz.js` resolves to `bar/foo`.

4. **Linear-scan reverse lookup.** Finding a `ModuleInfo` from a `v8::Module` handle
   requires iterating all entries. This is O(n) in the number of registered modules.

5. **Single evaluation callback for all synthetic modules.** The
   `evaluateSyntheticModuleCallback` is a global function shared by all synthetic
   modules. It relies on the reverse lookup to determine which module is being
   evaluated.

6. **No URL-based resolution.** The legacy registry uses `kj::Path` for specifiers,
   not URLs. There is no URL normalization, no query parameter handling, and no
   protocol-based dispatch. Known prefixes (`node:`, `cloudflare:`, `workerd:`) are
   handled as special cases in the resolve callback.

7. **Module type is part of the key.** The same path can exist as both BUNDLE and
   BUILTIN, with BUNDLE taking priority in DEFAULT resolution. This enables user
   code to shadow built-in modules.

8. **Mutable registry.** Entries can be added at any time (fallback service, lazy
   compilation). The `info` field within entries mutates from source/callback to
   `ModuleInfo` on first access.

9. **Top-level await handling.** After V8's `Evaluate` returns, `instantiateModule`
   runs microtasks and checks the promise state. If the promise is still pending,
   it throws — workerd does not support long-lived TLA during module loading (the
   worker must be fully initialized before serving requests).

10. **require() is module evaluation + namespace extraction.** The `requireImpl`
    function calls the same `instantiateModule` path as ESM imports, then extracts
    the appropriate export(s) from the module namespace. CJS modules are synthetic
    modules under the hood.
