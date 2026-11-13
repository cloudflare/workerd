# src/pyodide/

## OVERVIEW

Python Workers runtime layer. Replaces Pyodide's loader with a minimal substitute adding memory snapshot support. TS+Python; modules registered as `pyodide-internal:*` BUILTIN modules. The per-version Pyodide bundle built by `bundle/` contains only the patched Emscripten glue; its `pyodide-internal:*` imports resolve against `pyodide_static` at runtime.

## KEY COMPONENTS

| File/Dir                                      | Role                                                                                                  |
| --------------------------------------------- | ----------------------------------------------------------------------------------------------------- |
| `python-entrypoint-helper.ts`                 | BUILTIN module backing the USER `python-entrypoint.js`; request dispatch, handler wiring              |
| `internal/python.ts`                          | Core bridge: Emscripten init, Pyodide bootstrap, snapshot orchestration                               |
| `internal/snapshot.ts`                        | Memory snapshot collect/restore; baseline vs dedicated snapshot types                                 |
| `internal/setupPackages.ts`, `loadPackage.ts` | Package mounting, sys.path, vendor dir setup                                                          |
| `internal/tarfs.ts`                           | Read-only Emscripten filesystem backing site-packages / dynlib mounts                                 |
| `bundle/`                                     | Builds the per-version Pyodide bundle: `python_bundles.bzl`, `patch_pyodide_asm.ts`, `pack_python_packages.py` (stdlib wheels → `PythonPackages` message per `python_packages.capnp`), `python-lock/` |
| `internal/topLevelEntropy/`                   | TS+Python: patches `getRandomValues` with deterministic entropy during import, reseeds before request |
| `internal/emscriptenSetup.ts`                 | Instantiates the Emscripten module from the bundle's `pyodideRuntime-internal:pyodide.asm`            |
| `internal/builtin_wrappers.ts`                | Functions the patched Emscripten glue imports; released bundles import these by name                  |
| `internal/workers-api/`                       | Python SDK package (frozen)                                            |
| `scripts/`                                    | Standalone dev/CI scripts (not used by bazel): `make_snapshots.py`, `upload_bundles.py`, `create_vendor_zip.py` |
| `internal/metadata.ts`                        | Config flags: `IS_WORKERD`, `MAIN_MODULE_NAME`, etc.                                                  |
| `pyodide_extra.capnp`                         | Cap'n Proto schema for Pyodide bundle metadata                                                        |

## Python SDK

Python SDK (`internal/workers-api/`) now lives in [cloudflare/workers-py](https://github.com/cloudflare/workers-py) and is installed from PyPI. Keep existing code for backward compatibility; new features go to workers-py.

## BUNDLE AND BASELINE SNAPSHOT UPDATES

The code in this directory ships as three separately versioned artifacts:

| Artifact | Contents | How it ships |
| --- | --- | --- |
| `pyodide_static` | `python-entrypoint-helper.ts`, `internal/**`, `types/` | Compiled into the workerd binary. Ships with each runtime release. |
| Pyodide bundle (`pyodide_<version>_<date>_<backport>.capnp.bin`) | Built by `bundle/`: `pyodide.asm.wasm`, the Emscripten glue patched by `bundle/patch_pyodide_asm.ts` (module `pyodideRuntime-internal:pyodide.asm`), `python_stdlib.zip`, `python_packages.bin`. It contains no TypeScript from this directory. | Fetched from R2 for every version except `development`, pinned by `backport` and `integrity` in `build/python_metadata.bzl`. Only `development` uses the bundle built from the current tree. |
| Baseline snapshot | Wasm linear memory captured by `maybeCollectBaselineSnapshot()` in `internal/python.ts` | Fetched from R2 by `baseline_snapshot_hash` in `build/python_metadata.bzl`. |

### Bundle

Upload new bundles when a change touches `bundle/` or the Pyodide release in `PYODIDE_VERSIONS`
(`build/python_metadata.bzl`). Nothing else changes a bundle's contents.

The patched glue imports `pyodide-internal:builtin_wrappers` at runtime, so the bundle depends on
static code only through `internal/builtin_wrappers.ts`. Static code depends on the bundle only
through `_createPyodideModule`, the `Module` object it creates, and the glue function names that
`checkCallee()` in `builtin_wrappers.ts` allows.

### Baseline snapshot

The baseline snapshot captures Python state right after the runtime initializes. Python code
that ran before the snapshot was collected keeps its old behavior after a restore, even once
the source changes. Regenerate the snapshot when a change affects the state at collection time:

- `internal/topLevelEntropy/*.py`: imported, and `before_top_level()` runs, before collection
- `snapshotImports` in `src/workerd/api/pyodide/pyodide.c++`, `memorySnapshotDoImports()`,
  `adjustSysPath()`
- `collectSnapshot()` in `internal/snapshot.ts`, when the restore side needs the new metadata
- A bundle change that alters linear memory: new `pyodide.asm.wasm`, new stdlib or shared
  libraries, or a `bundle/patch_pyodide_asm.ts` patch to the code that creates or restores
  memory state

No regeneration is needed for:

- Code that runs after baseline collection: `introspection.py`, `workers-api/`,
  `setup_python_search_path.py`, `python-entrypoint-helper.ts`, request handling
- Changes to JS-only state: filesystem mounts, JS glue, and `emscriptenSetup.ts` or
  `builtin_wrappers.ts` changes that don't touch linear memory

### Dedicated snapshots

Dedicated snapshots are per-Worker and are never regenerated. Snapshots that are already in
production must still restore. Keep
restore code in `snapshot.ts` backward compatible with existing metadata and memory layouts
instead of regenerating those fixtures.

A dedicated snapshot also keeps every Python module imported before it was collected, the
runtime's own included: after a restore, `import` returns the snapshot's copy from `sys.modules`,
so a change to such a module doesn't reach Workers that restore an older snapshot.

## TESTING

Tests live in `src/workerd/server/tests/python/`. `py_wd_test.bzl` macro: expands `%PYTHON_FEATURE_FLAGS` template, handles multiple Pyodide versions, snapshot generation/loading, per-version compat flag isolation. Tests are `size="enormous"` by default. Each test generates variants per supported Pyodide version (`0.26.0a2`, newer).

When Python changes require new snapshots, run tests locally without creating or loading snapshots:

```sh
bazel test --config=regenerate-snapshots //src/workerd/server/tests/python:<target>
```
