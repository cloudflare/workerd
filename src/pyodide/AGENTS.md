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


## TESTING

Tests live in `src/workerd/server/tests/python/`. `py_wd_test.bzl` macro: expands `%PYTHON_FEATURE_FLAGS` template, handles multiple Pyodide versions, snapshot generation/loading, per-version compat flag isolation. Tests are `size="enormous"` by default. Each test generates variants per supported Pyodide version (`0.26.0a2`, newer).

When Python changes require new snapshots, run tests locally without creating or loading snapshots:

```sh
bazel test --config=regenerate-snapshots //src/workerd/server/tests/python:<target>
```
