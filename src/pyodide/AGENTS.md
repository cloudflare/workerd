# src/pyodide/

## OVERVIEW

Python Workers runtime layer. Replaces Pyodide's loader with a minimal substitute adding memory snapshot support. TS+Python; modules registered as `pyodide-internal:*` BUILTIN modules. `pool/emscriptenSetup.ts` runs in vanilla V8 isolate -- CANNOT import C++ extension modules.

## KEY COMPONENTS

| File/Dir                                      | Role                                                                                                  |
| --------------------------------------------- | ----------------------------------------------------------------------------------------------------- |
| `python-entrypoint-helper.ts`                 | BUILTIN module backing the USER `python-entrypoint.js`; request dispatch, handler wiring              |
| `internal/python.ts`                          | Core bridge: Emscripten init, Pyodide bootstrap, snapshot orchestration                               |
| `internal/snapshot.ts`                        | Memory snapshot collect/restore; baseline vs dedicated snapshot types                                 |
| `internal/setupPackages.ts`, `loadPackage.ts` | Package mounting, sys.path, vendor dir setup                                                          |
| `internal/tar.ts`, `tarfs.ts`                 | Tar archive parsing + read-only filesystem for bundles                                                |
| `internal/topLevelEntropy/`                   | TS+Python: patches `getRandomValues` with deterministic entropy during import, reseeds before request |
| `internal/pool/`                              | Emscripten setup in plain V8 isolate; `emscriptenSetup.ts` has NO access to C++ extensions            |
| `internal/workers-api/`                       | Python SDK package (`pyproject.toml` + `uv.lock` managed)                                             |
| `internal/metadata.ts`                        | Config flags: `IS_WORKERD`, `LOCKFILE`, `MAIN_MODULE_NAME`, etc.                                      |
| `pyodide_extra.capnp`                         | Cap'n Proto schema for Pyodide bundle metadata                                                        |

## TESTING

Tests live in `src/workerd/server/tests/python/`. `py_wd_test.bzl` macro: expands `%PYTHON_FEATURE_FLAGS` template, handles multiple Pyodide versions, snapshot generation/loading, per-version compat flag isolation. Tests are `size="enormous"` by default. Each test generates variants per supported Pyodide version (`0.26.0a2`, newer).
