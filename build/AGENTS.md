# build/ — Bazel Build Rules

## OVERVIEW

Custom Bazel rules (`wd_*` macros) for C++, TypeScript, Rust, Cap'n Proto, and test orchestration. Uses bzlmod (`MODULE.bazel`), not WORKSPACE. This is build system definitions, NOT build output (`bazel-bin/`).

## KEY RULES

| Rule                                       | Purpose                                                                                           |
| ------------------------------------------ | ------------------------------------------------------------------------------------------------- |
| `wd_cc_library.bzl`                        | Wraps `cc_library`; `strip_include_prefix="/src"`, arch-specific CPU flags (CRC32C)               |
| `wd_cc_binary.bzl`                         | Wraps `cc_binary`; macOS dead-strip linkopts; creates `_cross` alias for prebuilt arm64 binaries  |
| `wd_cc_embed.bzl`                          | Binary/text data -> C++ via C23 `#embed`; auto-detects text vs binary by extension                |
| `wd_cc_benchmark.bzl`                      | Google Benchmark wrapper; generates CSV report genrule                                            |
| `wd_test.bzl`                              | `.wd-test` config test runner; generates up to 3 variants per test                                |
| `kj_test.bzl`                              | C++ unit test wrapper; also generates test variants                                               |
| `wpt_test.bzl`                             | Web Platform Tests; generates JS runner + `.wd-test` config from WPT tree; delegates to `wd_test` |
| `wd_ts_bundle.bzl`                         | TypeScript compilation + JS bundle generation                                                     |
| `wd_js_bundle.bzl`                         | JS bundle -> Cap'n Proto `Modules.Bundle` embedding via generated `.capnp`                        |
| `wd_capnp_library.bzl`                     | Cap'n Proto schema compilation                                                                    |
| `wd_rust_crate.bzl` / `wd_rust_binary.bzl` | Rust build rules                                                                                  |
| `lint_test.bzl`                            | ESLint integration                                                                                |
| `//tools/clang-tidy:workerd-lint`          | Custom clang-tidy plugin (source: `tools/clang-tidy/workerd-lint.c++`); ships the `jsg-visit-for-gc`, `workerd-consume`, and `workerd-unsafe-continuation-capture` checks |

**Conventions:**

- `_cross` alias pattern: every `wd_cc_binary` gets a `name_cross` alias selecting prebuilt arm64 or source build
- Test tags: `off-by-default`, `requires-container-engine`, `no-asan`, `no-coverage`
- Variant generation controllable per-test via `generate_*_variant` booleans
- `BUILD.*` files: overlay build files for third-party deps (sqlite3, zlib, simdutf, pyodide, wpt)

## CLANG-TIDY PLUGIN

`//tools/clang-tidy:workerd-lint` builds a shared-object clang-tidy plugin
that adds workerd-specific static checks:

- `jsg-visit-for-gc`: flags JSG resource types whose visitable fields
  (`jsg::Ref`, `jsg::JsRef`, `jsg::V8Ref`, `jsg::Function`, `jsg::Promise`,
  `jsg::BufferSource`, `jsg::Value`, etc., plus `kj::Maybe`/`Array`/`Vector`/
  `OneOf` and `jsg::Optional` wrappers thereof) are missing from `visitForGc()`.
- `workerd-consume`: flags calls to methods annotated with `WD_CONSUME` when
  the call is made directly through `kj::Ptr` instead of through
  `consume(kj::mv(ptr))->method(...)`.
- `workerd-unsafe-continuation-capture`: flags lambdas passed to async sinks
  (e.g. `kj::Promise::then`) that capture bare references, raw pointers, or
  non-owning views.

Usage:

- Run via `just clang-tidy <target>` (e.g., `just clang-tidy //src/workerd/api/...`).
- Plugin sources live in `tools/clang-tidy/workerd-lint.c++` and
  `tools/clang-tidy/unsafe-continuation-capture.c++`, built as a
  `cc_shared_library` target `//tools/clang-tidy:workerd-lint`. The sources are
  also exported via `exports_files` so downstream projects can rebuild
  against their own clang/LLVM headers.
- The clang-tidy binary itself is published to `cloudflare/workerd-tools`
  releases (see `deps/build_deps.jsonc`, entries `clang_tidy_*`); the matching
  `*_dev.tar.xz` archive provides the clang/LLVM headers needed to build the
  plugin out-of-tree. Available for Linux amd64/arm64 and macOS arm64; a
  single archive (linux-amd64) serves all platforms since the AST-matching
  plugin doesn't depend on the arch-specific config macros that vary.
- Wrapper script `build/tools/clang_tidy/clang_tidy_wrapper.sh` loads the
  plugin via `--load=`.
- Suppress an intentional non-visit with `// NOLINT(jsg-visit-for-gc)` plus a
  comment explaining why the field is safe to skip (see `src/workerd/api/streams/queue.h`
  for `ByteQueue::Entry::store` and `src/workerd/api/node/diagnostics-channel.h`
  for `Channel::name`).

### Incremental check rollout

Some checks produce many warnings on existing code and need incremental rollout.
The `CHECK_PATH_FILTERS` dict in `build/tools/clang_tidy/check_path_filters.bzl`
supports this:

1. Add the check to `.clang-tidy` Checks list
2. Add an entry to `CHECK_PATH_FILTERS` with an empty list (runs nowhere)
3. Add packages as they are cleaned up
4. Remove the entry once fully rolled out (runs everywhere)

Example:

```python
CHECK_PATH_FILTERS = {
    "workerd-unsafe-continuation-capture": [
        "//src/workerd/io",
        "//src/workerd/api",
    ],
}
```

Package prefixes match themselves and all subpackages (`//src/workerd/io`
matches `//src/workerd/io:*` and `//src/workerd/io/subdir:*`).

To run a filtered check everywhere during development:

```bash
bazel build --config=clang-tidy-unsafe-continuation-capture //src/...
```

## DEPENDENCY MANAGEMENT

Lives in `deps/`. Uses jsonc manifests + codegen:

- `deps.jsonc`, `build_deps.jsonc`, `shared_deps.jsonc` — dependency specifications
- `update-deps.py [dep_name]` — fetches latest versions, computes hashes, regenerates `gen/` MODULE.bazel fragments
- `gen/` — **autogenerated**; do not hand-edit
- `*.MODULE.bazel` (e.g., `rust.MODULE.bazel`, `v8.MODULE.bazel`) — included by root `MODULE.bazel`
- `workerd-v8/` — separate Bazel module wrapping V8 dependency

Pyodide package metadata lives in `build/python_metadata.bzl`; the checked-in, pre-filtered
package lock files live in `src/pyodide/python-lock/`.
