# Dylint build adapter

This directory supplies source-build overlays, a provider-driven Bazel aspect,
and fixture rules for
[workerd's custom Rust lint collection](../../../tools/rust-lints/README.md).
The custom driver is separate from Clippy. `--config=dylint` enables opt-in
custom checks on the pinned nightly graph; `--config=lint` enables Clippy and
rustfmt without custom checks.

## Dependencies and compiler

`build/deps/build_deps.jsonc` tracks upstream Dylint and libloading without
freeze fields. `just update-deps dylint` and `just update-deps rust_lint_libloading`
advance the generated, checksummed source revisions. Do not hand-edit generated
repository definitions. The third-party Rust resolver in `deps/rust` supplies
their Rust dependencies.

`build/deps/rust.MODULE.bazel` is the single registration for stable and the
existing pinned nightly used by rustfmt and sanitizer builds. It enables dev
components for compiler-private tooling; rules_rust may also download those
components for the stable distribution in this shared registration. Stable
production builds and standalone `--config=clippy` use their existing compiler
and flags.

The rules_rust stdlib-link patch snapshots the `rust-std` component's rlibs
before `rustc-dev` installs compiler-private archives in the same sysroot.
All compiler inputs remain available, but only runtime stdlib archives are
exported to native linkers. This keeps rustc's jemalloc out of tcmalloc-linked
C++ binaries. Preserve that separation when upgrading rules_rust.

`BUILD.dylint` compiles the upstream driver and internal library without source
patches. It enables the upstream `rustup` feature to provide `is_rustc`, but does
not execute Cargo/rustup discovery. `cargo_toml_env_vars` supplies version metadata
from the upstream manifest rather than a hard-coded version string.
`BUILD.libloading` enables the upstream `std` feature.

No upstream driver build script runs. `extra_symbols.bzl` provides its expected
`OUT_DIR/extra_symbols.rs` as a declared tree artifact containing an empty symbol
table. This collection does not load Clippy libraries or link `clippy_utils`;
adding those would require compiler-matched, declared symbol-generation inputs.
Driver and collection use `rustc_private` and `-Cprefer-dynamic` to share the
compiler runtime.

## Bazel fixture actions

`fixture.bzl` discovers root `.rs` fixtures under `tests/explicit_unsafe/`, finds
optional matching `.out` files, and groups the generated targets into a test
suite. Most fixtures compile in both library and test mode. The active/inactive
test-module fixtures explicitly select the configuration they exercise.

For each fixture Bazel owns the command, flags, library selection, environment,
inputs, output marker, and execution-platform dependencies. Compilation and
expected-output comparison happen in a `DylintFixture` build action. The test
only verifies its success marker. A mismatch therefore fails the build before
the marker can be published or cached.

Declared inputs include driver and lint library, generated sysroot, compiler
and runtime libraries, sources, expected output, actual CXX dependency/proc-macro
artifacts, and the rules_python comparator's interpreter/runfiles. Updating any
of those invalidates the corresponding action normally.

The comparator only executes the configured compiler command, normalizes errors,
and compares one optional output file. It does not discover fixtures, resolve
crates, choose configurations, or group tests. Normalization removes sandbox
path prefixes and traversal order while preserving messages, filenames, error
codes, and duplicate diagnostics. Aborting summaries and unrelated warnings are
not part of the golden output.

`DYLINT_LIBS` is explicit JSON; `--sysroot` and runtime library directories are
explicit action arguments/environment. Actions do not inherit user Cargo,
rustup, or Dylint configuration, and do not resolve dependencies or fetch source.
Bazel's normal managed repository rules handle dependency downloads.

Upstream Dylint treats malformed/empty library configuration as no libraries.
The action passes `-Dunknown_lints` together with
`-Fexplicit_unsafe_policy`, making an unregistered custom lint an error.
Separate Bazel targets test unset, malformed, empty, missing, and non-plugin
library configurations; each must fail to compile a valid crate.

The rule requires nightly and matching Linux x86_64 host/target platforms;
unsupported requests fail clearly. Arm64 and macOS support require loader and
platform packaging tests. `fixture.bzl` uses rules_rust crate/dependency providers
but its command construction is deliberately specific to the isolated fixtures.
The repository aspect uses the production rules_rust argument/input adapter
instead.

## Repository aspect

`rust_lint.bzl` consumes `CrateInfo` and `TestCrateInfo`, and propagates markers
through dependency, proc-macro, wrapped-test-crate, and C++ link-dependency edges.
It publishes only `workerd_rust_lint_checks`. It neither inhibits Clippy nor
reuses Clippy's flags, config, suppression tags, or output group.

The small version-coupled adapter calls rules_rust's private `collect_deps`,
`collect_inputs`, and `construct_arguments`. These preserve crate roots,
editions, features/cfg flags, aliases, compile data, proc macros, environment
files, and build-script output/flag files. Rustc lint configuration is retained,
but Clippy-specific configuration is not passed to Dylint. The adapter emits
its own metadata file and successful marker without overwriting ordinary Rust
outputs or diagnostic files.

Driver and collection are execution-configured tools. The configured nightly
sysroot, compiler runtime libraries, dependency artifacts, proc-macro data,
plugin, and source manifest are declared inputs/tools. Plugin paths and runtime
paths are explicit environment values; path mapping is intentionally not
advertised for these actions. No Cargo/rustup installation or dependency
resolution runs inside lint actions.

The source manifest contains all original authored files, mapped to their
compiler-visible staged paths. The aspect retains original source ownership
across wrapped `rust_test` crates. Generated schemas and external dependency
crates do not need local policies. The manifest is a declared action input;
source ownership changes invalidate lint actions normally. Bootstrap targets
are excluded from traversal checks to keep the tool graph acyclic.

`--config=dylint` checks all active authored in-tree sources without path
filters or an audit override. This includes imported CXX and authored `kj-rs*`.
Test-only roots are excluded using Bazel `testonly` metadata, while wrapped
`rust_test` targets inherit their production crate's source inventory. The lint
exempts test-gated modules and their descendants without skipping production
modules compiled in test mode.

`integration_test.bzl` exercises exactly the same production adapter but compares
JSON errors with optional `.out` goldens via the declared comparator. The test
aspect checks only fixture targets, while still propagating providers through
real dependencies; unrelated dependency policies are not part of a fixture's
golden output. The production aspect has no such restriction. Fixtures cover library/binary/proc-macro/shared-library providers, standalone and wrapped
tests, aliases, feature flags, environment files, real CXX input, macro-generated
helpers, authored includes, and generated/staged source boundaries. A successful
negative fixture does not cause the production aspect to accept lint errors.

## Commands

```sh
bazel test //tools/rust-lints:explicit_unsafe_policy_test //tools/rust-lints:loader_test \
  //tools/rust-lints/tests/integration:adapter_test \
  --config=dylint --test_output=errors
bazel build //tools/rust-lints/tests/integration:binary \
  //tools/rust-lints/tests/integration:passing_test \
  --config=dylint
bazel aquery 'mnemonic("WorkerdRustLint", //tools/rust-lints/tests/integration:binary)' \
  --config=dylint --include_artifacts
```

`--config=dylint` selects the pinned nightly for the whole Rust graph
and enables the aspect/output group.
`--config=lint` does not enable custom checks. Production builds and Clippy
remain stable.
