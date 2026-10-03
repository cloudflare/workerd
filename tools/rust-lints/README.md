# Custom Rust lint plugins

This collection contains workerd-specific rustc lints loaded dynamically by
[Dylint](https://github.com/trailofbits/dylint). It is separate from the bundled
Clippy driver; it does not add checks to stable Clippy.

`--config=lint` runs Clippy, Dylint, and rustfmt on the pinned nightly Rust graph.
`--config=dylint` runs only the custom checks. Production builds and the standalone
`--config=clippy` remain on stable. The existing Linux lint CI job uses
`--config=lint`, so both drivers enforce their checks there.

## Build and test

The supported execution and target platform for these fixtures is Linux x86_64.
The fixture rule rejects other platforms and stable toolchains rather than
silently skipping checks.

```sh
bazel build //tools/rust-lints:driver //tools/rust-lints:workerd_lints \
  --config=dylint
bazel test //tools/rust-lints:explicit_unsafe_policy_test //tools/rust-lints:loader_test \
  //tools/rust-lints/tests/integration:adapter_test \
  --config=dylint --test_output=errors

just rust-lint memory-cache
just rust-lint //src/workerd/server:workerd-cli

bazel build //src/workerd/server:workerd-cli --config=dylint \
  --output_groups=workerd_rust_lint_checks
```

`just rust-lint` uses `--config=dylint` with no path filters or audit override.
The configuration selects nightly and enables the custom aspect/output group;
bootstrap targets are excluded from checks to avoid dependency cycles.
`just clippy` uses the combined `--config=lint` graph. To run only stable Clippy,
use `bazel build <target> --config=clippy`.

Bootstrap targets are tagged `manual`, so normal wildcard builds/tests do not
attempt to compile compiler-private code with stable Rust. The existing
`build/deps/rust.MODULE.bazel` registration supplies the same pinned nightly used
by rustfmt and sanitizer builds, including `rustc-dev`. There is no second
nightly distribution or global `RUSTC_BOOTSTRAP`. Stable production builds and
standalone Clippy retain their existing compiler and flags.

Dylint and libloading are tracked in `build/deps/build_deps.jsonc`, without
freeze fields. The update script advances their generated, checksummed source
revisions. Their Rust dependencies use the existing third-party crate resolver
(`deps/rust`). See [the build adapter](../../build/tools/rust_lint/README.md) for
hermetic packaging and declared runtime inputs. Neither Dylint nor libloading
needs source patches. The rules_rust stdlib-link patch keeps compiler-private
archives available to Rust tooling without exporting rustc's allocator to native
application links.

## `explicit_unsafe_policy`

Every **active authored production module** must explicitly declare a local
unsafe-code policy. Test-only crates, modules, and their descendants are exempt. Accepted inner declarations are:

```rust
#![deny(unsafe_code)]
#![allow(unsafe_code)]
#![forbid(unsafe_code)]
```

Use `deny` for safe modules; preserve existing `forbid` restrictions. Use `allow`
only for reviewed unsafe or FFI islands, explaining the reason where it is not
evident. The lint never inserts a policy automatically.

A module-local outer attribute also counts, as does a multi-lint attribute that
explicitly names the bare built-in `unsafe_code` lint. Prefer inner attributes:

```rust
#![deny(unsafe_code)]

mod parser; // parser.rs also needs its own policy.

#[cfg(test)]
mod tests {} // No declaration required.

// The CXX bridge generates unsafe FFI declarations.
#[cxx::bridge]
mod ffi {
    #![allow(unsafe_code)]
    #![expect(
        clippy::allow_attributes,
        reason = "CXX emits an outer unsafe-code policy"
    )]
}
```

The requirement covers crate roots, inline and nested modules, external files
(including `mod.rs` and `#[path]`), and original authored CXX bridge input. An inherited policy, even `forbid`, does not count. Neither do
`warn(unsafe_code)`, `expect(unsafe_code)`, `deny(warnings)`, a policy on a
function, or `deny(unsafe_op_in_unsafe_fn)`.

`cfg_attr` policies count only when active in the compilation. Inactive modules
and their descendants are not checked, including files disabled by inner cfg
attributes. `include!` is not a new module boundary, but authored modules inside
the included source are checked. Modules synthesized solely by macros are
excluded, including helpers whose identifiers are supplied by a caller.

A module gated exclusively on `test` (including `cfg(all(test, ...))` and active
`cfg_attr` forms) is exempt along with its inline and out-of-line descendants.
`cfg(any(test, unix))` is not exclusively test code and still needs a declaration.
Bazel `testonly` roots are exempt, but wrapped `rust_test` targets retain checks
for their production crate's sources. Test helper libraries should use Bazel's
`testonly` metadata, not a filename-based exemption. These exemptions do not
change Rust's enforcement of inherited `deny`/`forbid` policies.

CXX renders inner module attributes as outer attributes on generated modules.
Pair a bridge's inner `allow(unsafe_code)` with a scoped
`expect(clippy::allow_attributes)` explaining that transformation (or reuse an
existing expectation on that bridge). Do not replace the unsafe policy with
`expect(unsafe_code)` or disable Clippy for the crate.

Rust's built-in `unsafe_code` lint enforces the selected policy. This custom
lint checks declaration presence, not transitive soundness. The collection's
default level is warning; the aspect and fixtures pass
`-Fexplicit_unsafe_policy` only to the custom driver so source-level
suppression cannot evade the declaration requirement.
No custom lint names are added to ordinary rustc invocations.

Each missing declaration produces one diagnostic. Inline diagnostics identify
the module name; external-module diagnostics identify the external file where
an inner declaration belongs. Help is non-automatic because both blindly
allowing and blindly denying unsafe code can be wrong.

## Compiler-pass design

The pre-expansion pass observes original authored modules before attribute
macros such as `cxx::bridge` replace their input. It configures local attributes
using rustc's cfg evaluator, and tracks attribute scopes so inactive parents
(including freshly loaded file roots) suppress their descendants.

An unloaded `mod foo;` cannot be checked yet: its file-level inner attributes
are not available. The post-expansion pass checks source modules after those
attributes have been attached. Both passes share an inventory keyed by the
source identifier span, preventing duplicate diagnostics. Generated item spans
are excluded; using the identifier's expansion status alone is insufficient
because a macro can splice a caller-provided identifier into a generated item.
Missing-policy diagnostics are deferred until the post-expansion crate traversal
finishes. The complete module tree then identifies test-only ancestry, including
loaded file roots with inner cfg attributes and descendants in other files.

The aspect supplies a declared authored-source manifest, derived from crate
providers and original Bazel source inputs. Only modules in those files are
checked. Generated schema files are excluded even when included into an
authored root. Authored sources staged beside generated files remain in scope,
as do authored include fragments supplied as `compile_data`. External crates
are excluded. Imported in-tree CXX and authored KJ integrations are checked;
there is no path-based production-source exclusion.

The fixtures use the real in-tree CXX proc macro and verify exact diagnostic
output, accepted/rejected attributes, inner/outer file policies, active/inactive
cfgs, include fragments, test mode, built-in unsafe enforcement, and driver
failure modes. Bazel discovers sources under `tests/explicit_unsafe/` and their
optional `<source-name>.out` expectations; there is no central case manifest.
See [the fixture guide](tests/explicit_unsafe/README.md).

## Adding checks and upgrading tooling

Keep one implementation file per check and register it from `lib.rs`. Use a
plain lint name such as `explicit_unsafe_policy`; a tool namespace would require
registration in normal stable builds. Add passing and failing fixtures before
enforcing a new check. The aspect deliberately uses its own
`workerd_rust_lint_checks` output group, not `clippy_checks`.

Compiler-private ABI compatibility requires the exact same rustc build for the
driver, collection, dependencies, and proc macros. On every Rust toolchain or
Dylint pin update, rebuild the two bootstrap targets and run the fixture test.
Update compiler API adaptations and declared build inputs together;
matching only an LLVM version is not enough.

## Source inventory and remaining coverage

The Bazel production crates include `src/rust/`, the server CLI and `workerd`
binary, and `src/workerd/tools:param_extractor_bin`. Generated Cap'n Proto roots
and empty generated dependency-resolver roots are not authored modules.

Imported CXX parser, proc-macro, and generator modules use `deny` except for
specific implementation islands: transparent borrowed key casts, the thread-local
configuration side table, and the optional clang AST memory mapping. CXX runtime
ABI/layout/ownership modules and authored KJ smart-pointer/future/waker bindings
explicitly allow their unsafe operations. Safe children and re-export modules
use `deny`; existing `forbid` restrictions are preserved. CLI descriptor handling
and dedicated bridges remain unsafe islands. Test-support packages use Bazel
`testonly` metadata and need no unsafe-policy migration.

Linux arm64/macOS arm64 packaging and additional host/target/feature configuration
coverage remain work to do.

Compiler-based coverage is configuration-dependent. These Linux fixtures do
not establish coverage of inactive Windows/macOS source or every feature
configuration, and do not justify a repository-wide “every module” guarantee.
