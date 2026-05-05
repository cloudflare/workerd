# V8 Module System Internals

Reference for reasoning about V8's module lifecycle, binding resolution, and evaluation
semantics. For how workerd interfaces with these V8 APIs, see
[new-module-registry.md](new-module-registry.md) (current implementation) and
[legacy-module-registry.md](legacy-module-registry.md) (legacy implementation).

All file paths are relative to the V8 source root
(`external/+http_archive+v8/`). Line numbers are approximate and may drift across V8
versions.

## Source Files

| File                                | Role                                                                                |
| ----------------------------------- | ----------------------------------------------------------------------------------- |
| `include/v8-script.h`               | Public `v8::Module`, `v8::ModuleRequest` API                                        |
| `include/v8-callbacks.h`            | `ModuleImportPhase`, host callback typedefs                                         |
| `src/objects/module.h`              | Internal `Module` base class (C++ header)                                           |
| `src/objects/module.cc`             | Base class dispatch: `Instantiate`, `Evaluate`, `ResolveExport`, namespace creation |
| `src/objects/module.tq`             | Torque layout for `Module`, `JSModuleNamespace`, `ScriptOrModule`                   |
| `src/objects/source-text-module.h`  | `SourceTextModule` header                                                           |
| `src/objects/source-text-module.cc` | ESM linking, resolution, evaluation, TLA                                            |
| `src/objects/source-text-module.tq` | Torque layout for `SourceTextModule`, `ModuleRequest`, `SourceTextModuleInfoEntry`  |
| `src/objects/synthetic-module.h`    | `SyntheticModule` header                                                            |
| `src/objects/synthetic-module.cc`   | Synthetic module instantiation, evaluation, `SetExport`                             |
| `src/objects/synthetic-module.tq`   | Torque layout for `SyntheticModule`                                                 |
| `src/objects/module-inl.h`          | Inline accessors, `UnorderedModuleSet`, `ModuleHandleHash`                          |
| `src/heap/factory.cc`               | `NewSourceTextModule`, `NewSyntheticModule` factory methods                         |
| `src/api/api.cc`                    | Public API bridge (`CompileModule`, `CreateSyntheticModule`, etc.)                  |

## Class Hierarchy and Heap Layout

The hierarchy is defined in Torque (`.tq` files), which generates C++ heap object code.
`Module` is abstract; only `SourceTextModule` and `SyntheticModule` are concrete.

### Module (abstract base — `module.tq`)

```
Module extends HeapObject {
  exports:                   ObjectHashTable   // export_name (String) → Cell
  hash:                      Smi               // identity hash for module-keyed maps
  status:                    Smi               // lifecycle state enum
  module_namespace:          Cell | Undefined   // cached JSModuleNamespace (lazy)
  deferred_module_namespace: Cell | Undefined   // for `import defer` (lazy)
  exception:                 Object            // error value if status == kErrored
  top_level_capability:      JSPromise | Undefined  // only set on SCC cycle roots
}
```

### SourceTextModule (ESM — `source-text-module.tq`)

```
SourceTextModule extends Module {
  code:             SharedFunctionInfo | JSFunction | JSGeneratorObject
  regular_exports:  FixedArray    // cell_index → Cell (positional)
  regular_imports:  FixedArray    // cell_index → Cell (positional)
  requested_modules: FixedArray   // i → Module (resolved dependency for request i)
  import_meta:      TheHole | JSObject   // lazily created on first access
  cycle_root:       SourceTextModule | TheHole
  async_parent_modules: ArrayList
  dfs_index:              Smi
  dfs_ancestor_index:     Smi
  pending_async_dependencies: Smi
  flags:            SmiTagged<SourceTextModuleFlags>
    has_toplevel_await:         1 bit
    async_evaluation_ordinal:  30 bits
}
```

### SyntheticModule (`synthetic-module.tq`)

```
SyntheticModule extends Module {
  name:             String       // display name (debugging/logging only)
  export_names:     FixedArray   // declared export name strings
  evaluation_steps: Foreign      // C++ function pointer (kSyntheticModuleTag)
}
```

### Namespace Objects

```
JSModuleNamespace extends JSSpecialObject {
  module: Module
}

JSDeferredModuleNamespace extends JSModuleNamespace {}
```

A `JSModuleNamespace` is the `import * as ns` object. It is created lazily by
`Module::GetModuleNamespace` and cached in the `module_namespace` Cell. Properties
are accessor-based: each export name maps to a `module_namespace_property_accessor`
that performs a live lookup through the exports table.

`JSDeferredModuleNamespace` is used for `import defer * as ns` and triggers
synchronous evaluation on first property access (see Import Phases below).

## The Export Table

The `exports` field is an `ObjectHashTable` mapping export name strings to `Cell`
objects. This is the core binding mechanism for the entire module system.

### How live bindings work

Both the exporting module and all importing modules resolve to the **same `Cell`
object**. When code in the exporting module updates a `let`/`var` export, it writes
to the Cell's value. All importers read from the same Cell and immediately see the
new value. There is no copy step.

### What the table contains at each stage

| Module Status | Contents of `exports` entries                                                                                                                                                                            |
| ------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| kUnlinked     | Empty (freshly allocated ObjectHashTable)                                                                                                                                                                |
| kPreLinking   | For SourceTextModule: regular exports → fresh Cells (value = TheHole), indirect exports → SourceTextModuleInfoEntry placeholders. For SyntheticModule: each export_name → fresh Cell (value = undefined) |
| kLinking      | Indirect exports progressively replaced with resolved Cells                                                                                                                                              |
| kLinked+      | All entries are Cells. TheHole value = uninitialized variable (will throw ReferenceError on access)                                                                                                      |

### ObjectHashTable rehashing

`ObjectHashTable::Put` may allocate a new backing table. After any `Put`, the caller
must re-read the module's exports handle. The implementation is careful about this:
every export-table mutation follows the pattern:

```cpp
exports = ObjectHashTable::Put(isolate, exports, name, cell);
module->set_exports(*exports);
```

## Lifecycle State Machine

### Internal States (`Module::Status` enum in `module.h`)

```
kUnlinked(0) → kPreLinking(1) → kLinking(2) → kLinked(3)
  → kEvaluating(4) → kEvaluatingAsync(5) | kEvaluated(6)
  any → kErrored(7)
```

Status transitions are monotonically increasing (`DCHECK_LE(status(), new_status)`)
**except**:

- `Reset` moves a module from kPreLinking/kLinking back to kUnlinked (on
  instantiation failure).
- `RecordError` moves any state to kErrored.

### Public API Mapping (`v8::Module::Status`)

| Internal         | Public          | Notes                                   |
| ---------------- | --------------- | --------------------------------------- |
| kUnlinked        | kUninstantiated |                                         |
| kPreLinking      | kInstantiating  |                                         |
| kLinking         | kInstantiating  |                                         |
| kLinked          | kInstantiated   |                                         |
| kEvaluating      | kEvaluating     |                                         |
| kEvaluatingAsync | kEvaluated      | Collapsed into kEvaluated in public API |
| kEvaluated       | kEvaluated      |                                         |
| kErrored         | kErrored        |                                         |

### The `code` field transitions (SourceTextModule only)

The `code` field is polymorphic. Its type tracks the module's progress:

| Status                                             | `code` type                                                   | Meaning                                     |
| -------------------------------------------------- | ------------------------------------------------------------- | ------------------------------------------- |
| kUnlinked, kPreLinking                             | `SharedFunctionInfo`                                          | Compiled but not yet linked                 |
| kLinking                                           | `JSFunction`                                                  | Created from SFI during `FinishInstantiate` |
| kLinked, kEvaluating, kEvaluatingAsync, kEvaluated | `JSGeneratorObject` (sync) or `JSAsyncFunctionObject` (async) | Created by `RunInitializationCode`          |
| kErrored                                           | `SharedFunctionInfo`                                          | Reverted by `RecordError`                   |

The revert on error is important: `RecordError` calls
`self->set_code(self->GetSharedFunctionInfo())` to strip the JSFunction/generator
and return to the minimal SFI state.

## SourceTextModule: Detailed Lifecycle

### 1. Creation — `Factory::NewSourceTextModule` (`factory.cc`)

Entry point: `ScriptCompiler::CompileModule` → compiles source to `SharedFunctionInfo`
→ `factory->NewSourceTextModule(sfi)`.

The SFI's `ScopeInfo` contains a `SourceTextModuleInfo` (`module-inl.h:73-74`) with:

- `module_requests()`: `FixedArray` of `ModuleRequest` (specifier + phase + import attributes)
- `regular_exports()`: triples of (local_name, cell_index, export_names)
- `regular_imports()`: `SourceTextModuleInfoEntry` (import_name, module_request, cell_index)
- `special_exports()`: indirect and star re-export entries
- `namespace_imports()`: namespace import entries

The factory allocates arrays for `regular_exports`, `regular_imports`, and
`requested_modules` (all initially empty/zeroed), sets status to kUnlinked, and
initializes all other fields to their zero/hole/undefined states.

### 2. Instantiation — `Module::Instantiate` (`module.cc`)

Two-phase process: `PrepareInstantiate` then `FinishInstantiate`. If either fails,
`ResetGraph` walks the entire dependency graph and resets all modules in
kPreLinking/kLinking back to kUnlinked.

#### Phase 1: PrepareInstantiate (`source-text-module.cc:363-481`)

Sets status to kPreLinking, then for each `ModuleRequest`:

**Evaluation/Defer phase imports**: Calls the embedder's `ResolveModuleCallback`
(specifier-based) or `ResolveModuleByIndexCallback` (index-based). The returned
`v8::Module` is stored in `requested_modules[i]`.

**Source phase imports** (`import source`): Calls the embedder's `ResolveSourceCallback`
or `ResolveSourceByIndexCallback`. The returned `v8::Object` is stored in
`requested_modules[i]` (not a Module — it's an opaque source representation).

Then **recurses** into all evaluation-phase dependencies (skipping source-phase).

After recursion, populates the export table:

- **Regular exports**: For each (cell_index, export_names), creates a Cell and inserts
  it into both `regular_exports[cell_index]` and `exports[name]` for each export name.
- **Indirect exports**: For each special_export with a non-undefined export_name,
  inserts the `SourceTextModuleInfoEntry` as a placeholder in `exports[name]`.
  This placeholder is later replaced by the resolved Cell during `FinishInstantiate`.

#### Phase 2: FinishInstantiate (`source-text-module.cc:594-687`)

Implements Tarjan's strongly connected components algorithm:

1. Creates `JSFunction` from SFI, stores in `code`. Sets status to kLinking.
2. Assigns DFS indices, pushes module onto SCC stack.
3. Recurses into dependencies (skipping source-phase). For each:
   - Calls `Module::FinishInstantiate` (which dispatches to the subclass).
   - If the dependency is still kLinking (on the stack), updates
     `dfs_ancestor_index` to detect the SCC.
4. Resolves **imports**: For each `regular_import`, calls `ResolveImport` which:
   - Source phase: wraps the requested object in a new Cell.
   - Evaluation/Defer phase with a name: calls `Module::ResolveExport` on the
     target to find the binding Cell.
   - Evaluation/Defer phase without a name (namespace import): calls
     `GetModuleNamespaceCell`.
   - Stores the resolved Cell in `regular_imports[cell_index]`.
5. Resolves **indirect exports**: For each special_export with a name, calls
   `ResolveExport` to replace the placeholder entry with the real Cell.
6. When an SCC root is found (`dfs_ancestor_index == dfs_index`), calls
   `MaybeTransitionComponent` which pops the entire SCC off the stack, runs
   `RunInitializationCode` on each (creating module context + generator), and
   transitions all to kLinked.

#### RunInitializationCode (`source-text-module.cc:483-505`)

Called during SCC completion (not during evaluation). Creates:

1. A `ModuleContext` from the module's scope info.
2. Sets the JSFunction's context to this new module context.
3. Calls the function, which returns a `JSGeneratorObject`.
4. Stores the generator in `code`.

This is the step where variable bindings are initialized (but module body code
has not run — the generator is paused at the start).

### 3. Export Resolution — `ResolveExport` / `ResolveImport`

#### ResolveExport (`source-text-module.cc:182-250`)

Given an export name, looks it up in the exports table:

- If it's a `Cell`: already resolved, return it.
- If it's a `SourceTextModuleInfoEntry`: an unresolved indirect export. Follow
  the chain via `ResolveImport` on the entry's module_request + import_name.
  On success, replace the entry in the exports table with the resolved Cell.
- If it's `TheHole`: the name wasn't found as a direct or indirect export.
  Fall through to `ResolveExportUsingStarExports`.

**Cycle detection**: Uses a `ResolveSet` (map of `Handle<Module>` → `UnorderedStringSet`).
Before recursing, inserts (module, export_name) into the set. If already present,
a cycle is detected. With `must_resolve=true`, this throws a SyntaxError
(`kCyclicModuleDependency`).

#### ResolveExportUsingStarExports (`source-text-module.cc:303-361`)

Walks all star exports (`special_exports` entries where `export_name` is undefined).
For each, tries `ResolveImport` with the target name. Collects results:

- If exactly one star export provides the name: uses that Cell.
- If multiple provide it with the same Cell: fine (diamond dependency).
- If multiple provide different Cells: throws `kAmbiguousExport`.
- The `default` export is never resolved through star exports (spec requirement).

### 4. Evaluation — `SourceTextModule::Evaluate` (`source-text-module.cc:890-940`)

1. Creates a `JSPromise` as the top-level capability (stored on the SCC cycle root).
2. Calls `InnerModuleEvaluation` which does a DFS:
   - Sets status to kEvaluating.
   - Gathers dependencies (with special handling for deferred imports via
     `GatherAsynchronousTransitiveDependencies`).
   - Recursively evaluates all evaluation-phase dependencies.
   - Tracks `pending_async_dependencies` and `async_parent_modules` for
     ordering async evaluation.
   - If the module has TLA or pending async deps: sets `async_evaluation_ordinal`
     and calls `ExecuteAsyncModule` if no pending deps remain.
   - If synchronous: calls `ExecuteModule` directly.
   - Transitions the SCC via `MaybeTransitionComponent` to kEvaluated (or
     kEvaluatingAsync if async).
3. Returns the top-level promise.

#### Synchronous execution (`ExecuteModule`, line 1193)

Resumes the `JSGeneratorObject` stored in `code` via `generator_next_internal`.
Returns the completion value from `JSIteratorResult`.

#### Asynchronous execution (`ExecuteAsyncModule`, line 1102)

The `code` field is a `JSAsyncFunctionObject`. Wires up `onFulfilled` /
`onRejected` callbacks (built from shared function infos stored in the native
context) onto a new Promise, then calls `InnerExecuteAsyncModule` which resumes
the async function via `async_module_evaluate_internal`.

#### Async completion (`AsyncModuleExecutionFulfilled`, line 943)

When an async module completes:

1. Sets status to kEvaluated, resolves the top-level promise.
2. Calls `GatherAvailableAncestors` — iterates `async_parent_modules`, decrements
   their `pending_async_dependencies`.
3. Any ancestor that reaches zero pending deps and has TLA: triggers
   `ExecuteAsyncModule`.
4. Any ancestor that reaches zero pending deps and is synchronous: runs
   `ExecuteModule`, transitions to kEvaluated.

The `async_evaluation_ordinal` provides a deterministic ordering (set sequentially
as modules enter async evaluation via `isolate->NextModuleAsyncEvaluationOrdinal()`).

### 5. Error Handling

`RecordError` (`module.cc:105-124`):

- For SourceTextModule: reverts `code` to the original SFI.
- Sets status to kErrored.
- Stores the exception, or `null` for termination exceptions.

Evaluation error propagation (`MaybeHandleEvaluationException`, line 860):

- If the exception is catchable by JS: records it on all modules in the SCC stack.
- If it's a termination exception: also records on all, but with `null` as the
  exception. Returns false to signal the caller that promise rejection should
  not happen.

Failed evaluations return a rejected promise. Re-evaluating an already-errored
module returns the same rejected promise (or creates a new one if no
top_level_capability exists).

## SyntheticModule: Detailed Lifecycle

### 1. Creation — `Factory::NewSyntheticModule` (`factory.cc:3605-3631`)

Entry point: `v8::Module::CreateSyntheticModule(isolate, module_name, export_names,
evaluation_steps)`.

- `export_names` are internalized strings stored in a `FixedArray`.
- `evaluation_steps` is a C++ function pointer stored as a `Foreign` with
  `kSyntheticModuleTag` (for V8 sandbox pointer tagging).
- Status set to kUnlinked. Exports table allocated but empty.

### 2. Instantiation

#### PrepareInstantiate (`synthetic-module.cc:72-88`)

For each export name in `export_names`:

1. Creates a new mutable `Cell` (initial value: `undefined`, stored as TheHole
   in the Cell's default constructor — but the implementation uses `NewCell()`
   which initializes to `undefined`).
2. Inserts (name → Cell) into the `exports` hash table.

There are no imports or dependencies to resolve.

#### FinishInstantiate (`synthetic-module.cc:93-104`)

Sets status to kLinked. If a namespace object was pre-created, ensures it's
populated.

SyntheticModules skip the DFS/SCC stack entirely — they go straight to kLinked
without receiving DFS indices.

### 3. Evaluation (`synthetic-module.cc:108-143`)

1. Sets status to kEvaluating.
2. Extracts the `evaluation_steps` function pointer from the Foreign object.
3. Calls the embedder callback: `evaluation_steps(context, module)`.
4. If the callback returns a value:
   - Sets status to kEvaluated.
   - If the return is a JSPromise, uses it as `top_level_capability`.
   - Otherwise, creates a new resolved Promise (backward compat for pre-TLA hosts).
5. If the callback throws:
   - `RecordError` is called → status becomes kErrored.
   - Returns empty MaybeHandle.

### 4. SetExport (`synthetic-module.cc:21-38`)

The embedder calls `v8::Module::SetSyntheticModuleExport(isolate, name, value)`
during the evaluation callback.

1. Looks up `name` in the `exports` hash table.
2. If not found (not in the original `export_names`): throws a ReferenceError
   (`kModuleExportUndefined`).
3. If found: sets `cell->set_value(export_value)`.

`SetExportStrict` is a variant that CHECKs instead of throwing — it's a legacy
method that will be removed.

### Synthetic module constraints

- SyntheticModules **cannot have imports** or dependencies of any kind.
- They **cannot re-export** from other modules.
- They **do not participate in SCC detection** (no DFS indices).
- They are always "leaf" nodes in the module graph.
- They can only be depended upon by SourceTextModules (or other embedder code).

## Import Phases (`ModuleImportPhase` — `v8-callbacks.h`)

```cpp
enum class ModuleImportPhase {
  kSource,      // 0  — import source x from "mod"
  kDefer,       // 1  — import defer * as ns from "mod"
  kEvaluation,  // 2  — standard import
};
```

Stored as a 2-bit field in `ModuleRequest::flags`.

### Source Phase (`kSource`)

- During `PrepareInstantiate`: calls `ResolveSourceCallback` instead of
  `ResolveModuleCallback`. The result is a `v8::Object` (not a Module) stored
  in `requested_modules[i]`.
- During `FinishInstantiate`: source-phase entries are skipped entirely (no linking).
- During `ResolveImport`: wraps the stored object in a Cell directly.
- During evaluation: source-phase entries are skipped.

### Defer Phase (`kDefer`)

- Resolution proceeds identically to kEvaluation (same callback, same Module result).
- During `ResolveImport`: if importing a namespace (`maybe_name` is empty), calls
  `GetModuleNamespaceCell(isolate, requested_module, ModuleImportPhase::kDefer)` which
  stores the namespace in `deferred_module_namespace` instead of `module_namespace`.
- Creates `JSDeferredModuleNamespace` instead of `JSModuleNamespace`.
- During evaluation: the deferred module's transitive async dependencies are gathered
  via `GatherAsynchronousTransitiveDependencies` and eagerly evaluated, but the
  synchronous module graph is not evaluated until first property access.

### Deferred Evaluation Trigger

`JSDeferredModuleNamespace::TriggersEvaluation` (`module.cc:520-533`) is called
by the property lookup machinery. Returns true if:

- The holder is a `JSDeferredModuleNamespace`, AND
- The property is not `then` or a Symbol (thenable check / iterator protocol), AND
- The module's status is not yet kEvaluated.

When triggered, `EvaluateModuleSync` (`module.cc:482-518`):

1. Checks `ReadyForSyncExecution` — recursively verifies no TLA in the transitive
   graph. Throws TypeError if not ready.
2. Calls `Module::Evaluate`.
3. If the resulting promise is rejected: throws the rejection reason.
4. Asserts the promise is fulfilled (synchronous completion guaranteed by the
   ReadyForSyncExecution check).

## Namespace Object Creation — `Module::GetModuleNamespace` (`module.cc:343-424`)

Lazy, cached in the `module_namespace` (or `deferred_module_namespace`) Cell.

1. If the Cell already has a JSModuleNamespace value, return it.
2. For SourceTextModules: call `FetchStarExports` to collect transitive star exports.
3. Collect all keys from the exports hash table.
4. Sort alphabetically (spec requirement: `@@unscopables`, then lexicographic).
5. Create a `JSModuleNamespace` (or `JSDeferredModuleNamespace`).
6. Transition to dictionary mode, add each name as an accessor property using
   `module_namespace_property_accessor`.
7. Prevent extensions, optimize as prototype (enables Turbofan inlining via
   `PrototypeInfo::module_namespace`).

Property access on the namespace goes through the accessor, which calls
`JSModuleNamespace::GetExport` → looks up the Cell in `exports`, reads its value.
If the value is TheHole (uninitialized binding), throws ReferenceError.

## No Built-in Module Registry

V8 does not maintain any specifier-to-Module mapping. The embedder is responsible
for:

- Storing compiled Module objects keyed by specifier.
- Returning the correct Module from `ResolveModuleCallback` /
  `ResolveModuleByIndexCallback`.
- Ensuring the same Module object is returned for the same specifier (deduplication).
- Implementing any URL resolution or path normalization.

V8 calls the resolve callback for every `import` statement encountered during
`PrepareInstantiate`. If the embedder returns a different Module for the same
specifier on different calls, V8 will treat them as distinct modules.

## The ResolveModuleCallback Contract

```cpp
using ResolveModuleCallback = MaybeLocal<Module> (*)(
    Local<Context> context,
    Local<String> specifier,
    Local<FixedArray> import_attributes,  // [key, value, offset, ...]
    Local<Module> referrer);

using ResolveModuleByIndexCallback = MaybeLocal<Module> (*)(
    Local<Context> context,
    size_t module_request_index,
    Local<Module> referrer);
```

The by-index variant avoids redundant string comparisons — the index corresponds
directly to the `module_requests()` array position in the referrer's
`SourceTextModuleInfo`.

The callback must:

- Return a Module (compiled but not necessarily instantiated) for evaluation-phase.
- Return empty MaybeLocal to signal an error (must have thrown on the isolate).

V8 will then recursively call `PrepareInstantiate` on the returned module, so the
embedder does not need to pre-instantiate dependencies.

## Key Invariants

1. **Status is monotonically increasing** (except Reset on failed instantiation and
   RecordError). Code that checks `status >= kLinked` is checking "at least linked."

2. **SyntheticModules skip SCC detection.** They transition directly from kPreLinking
   to kLinked in `FinishInstantiate` without DFS indices. In
   `SourceTextModule::FinishInstantiate`, encountering a kLinking module is asserted
   to be a SourceTextModule (line 639-644).

3. **Cells are shared, not copied.** The same Cell appears in `regular_exports`,
   `regular_imports` of dependent modules, and the `exports` hash table. All three
   are references to the same heap object.

4. **Error on errored re-evaluate.** If `Module::Evaluate` is called on a kErrored
   module, it returns a rejected promise immediately (`module.cc:276-290`) without
   re-running any code.

5. **top_level_capability is only set on SCC cycle roots.** For evaluating modules
   that are part of a cycle, only the root module (lowest DFS index in the SCC) has
   a top_level_capability. `Module::Evaluate` checks for an existing capability
   before dispatching to subclass Evaluate, and returns it if present (re-evaluation
   returns the same promise).

6. **import.meta is lazily initialized.** First access triggers
   `RunHostInitializeImportMetaObjectCallback`. The result is cached with
   acquire/release store semantics (`kAcquireLoad` / `kReleaseStore`) because
   import.meta may be accessed from background compilation threads.

7. **RecordError reverts SourceTextModule code to SFI.** This ensures GC can collect
   the JSFunction and generator, and prevents dangling references to a partially
   initialized module context.

8. **Termination exceptions are stored as null.** The exception field contains the
   JS-visible exception if catchable, or `null` for termination. Callers use
   `isolate->is_catchable_by_javascript()` to distinguish.

## Common Patterns for Embedders

### Creating an ESM

```cpp
v8::ScriptOrigin origin(resource_name, /*line=*/0, /*col=*/0,
                         /*is_shared_cross_origin=*/false,
                         /*script_id=*/-1, /*source_map=*/{},
                         /*is_opaque=*/false, /*is_wasm=*/false,
                         /*is_module=*/true);  // ← must be true
v8::ScriptCompiler::Source source(code_string, origin);
v8::Local<v8::Module> module =
    v8::ScriptCompiler::CompileModule(isolate, &source).ToLocalChecked();
```

### Creating a SyntheticModule

```cpp
v8::Local<v8::String> export_names[] = { v8_str("default"), v8_str("foo") };
v8::Local<v8::Module> module = v8::Module::CreateSyntheticModule(
    isolate, v8_str("synth:my-module"),
    v8::MemorySpan<const v8::Local<v8::String>>(export_names, 2),
    [](v8::Local<v8::Context> context, v8::Local<v8::Module> module)
        -> v8::MaybeLocal<v8::Value> {
      v8::Isolate* isolate = context->GetIsolate();
      module->SetSyntheticModuleExport(isolate, v8_str("default"), v8_int(42));
      module->SetSyntheticModuleExport(isolate, v8_str("foo"), v8_str("bar"));
      return v8::Boolean::New(isolate, true);
    });
```

### Instantiation + Evaluation

```cpp
module->InstantiateModule(context, ResolveCallback).Check();
v8::Local<v8::Value> result = module->Evaluate(context).ToLocalChecked();
// result is a Promise (always, even for sync modules)
```

### Checking completion

For synchronous modules, the returned Promise is already settled:

```cpp
v8::Local<v8::Promise> promise = result.As<v8::Promise>();
CHECK_EQ(promise->State(), v8::Promise::kFulfilled);
```

For async modules (TLA), the Promise may be pending and will settle later
via microtask processing.
