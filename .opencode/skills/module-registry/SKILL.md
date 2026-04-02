---
name: module-registry
description: Load when working with the module registry in workerd — reading, modifying, debugging, or reviewing module resolution, compilation, evaluation, or registration code. Provides pointers to three reference documents covering the legacy registry, V8 module internals, and the new registry design.
---

## Module Registry Context

When working with any code related to module resolution, compilation, evaluation,
or registration in workerd, you **must** read the relevant reference documents
before making changes or providing analysis. These documents capture the design
decisions, data flow, thread safety model, and integration points that are not
obvious from reading the source alone.

### Reference Documents

Read these documents (using the Read tool) before proceeding with any module
registry work:

1. **`docs/reference/detail/new-module-registry.md`** — The new `jsg::modules::ModuleRegistry`
   implementation. Covers the two-layer architecture (shared `ModuleRegistry` +
   per-isolate `IsolateModuleRegistry`), bundle types, resolution priority,
   compile cache, evaluation flow, and thread safety model. **Read this first**
   for any work on the new registry.

2. **`docs/reference/detail/legacy-module-registry.md`** — The legacy
   `ModuleRegistryImpl<TypeWrapper>` implementation. Covers the per-isolate
   design, `ModuleInfo` structure, `DynamicImportHandler`, and how it interfaces
   with V8. **Read this** when working on code that still uses the legacy path,
   or when understanding the dispatch between legacy and new.

3. **`docs/reference/detail/v8-module-internals.md`** — How V8 handles modules
   internally. Covers `v8::Module` status transitions, `InstantiateModule`,
   `Evaluate`, `SyntheticModule`, resolve callbacks, and `import.meta`. **Read
   this** when debugging V8 module state issues, understanding callback
   contracts, or working on the boundary between workerd and V8.

### When to Read Which

| Task                                              | Documents to read                               |
| ------------------------------------------------- | ----------------------------------------------- |
| Modifying new registry resolution or caching      | new-module-registry, v8                         |
| Adding a new built-in module                      | new-module-registry, legacy-module-registry     |
| Debugging module-not-found errors                 | all three                                       |
| Modifying legacy registry code                    | legacy-module-registry, v8                      |
| Understanding legacy-to-new dispatch              | all three                                       |
| Working on `import.meta` behavior                 | new-module-registry, v8                         |
| Working on CJS/require() interop                  | new-module-registry, legacy-module-registry     |
| Debugging module evaluation failures              | new-module-registry, legacy-module-registry, v8 |
| Reviewing a PR that touches module code           | all three                                       |
| Working on compile cache or cross-isolate sharing | new-module-registry                             |
| Working on dynamic import                         | all three                                       |
| Working on source phase imports (Wasm)            | new-module-registry, v8                         |

### Key Source Files

These are the primary files involved in module handling:

- `src/workerd/jsg/modules-new.h` / `.c++` — New registry implementation
- `src/workerd/jsg/modules.h` / `.c++` — Legacy registry implementation
- `src/workerd/jsg/resource.h:1913–1922` — Legacy vs new dispatch at context creation
- `src/workerd/jsg/jsg.c++:357–395` — `Lock::resolveModule` dispatch
- `src/workerd/jsg/setup.h:291–297` — `isUsingNewModuleRegistry()` flag
- `src/workerd/io/worker-modules.h` — `newWorkerModuleRegistry` construction
- `src/workerd/api/modules.h` — Built-in module registration (both legacy and new)
- `src/workerd/jsg/observer.h` — `ResolveObserver` metrics interface

### Procedure

1. **Read** the relevant reference documents listed above for your task.
2. **Identify** whether the code path uses the new or legacy registry (check
   `isUsingNewModuleRegistry()` usage in the surrounding code).
3. **Proceed** with your work, using the reference documents to inform your
   understanding of data flow, thread safety, and integration contracts.

Do not skip reading the reference documents. The module registry has subtle
invariants around thread safety, V8 module status transitions, and evaluation
context (IoContext suppression) that are easy to violate without full context.
