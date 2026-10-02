# Provider-driven custom Rust lint fixtures

```sh
bazel test //tools/rust-lints/tests/integration:adapter_test \
  --config=dylint --test_output=errors

# Ordinary aspect: these must pass.
bazel build //tools/rust-lints/tests/integration:binary \
  //tools/rust-lints/tests/integration:passing_test \
  //tools/rust-lints/tests/integration:shared --config=dylint

# Ordinary aspect: these must fail on missing policies.
bazel build //tools/rust-lints/tests/integration:missing \
  //tools/rust-lints/tests/integration:missing_test \
  --config=dylint --keep_going
```

The targets are manual to keep synthetic inputs out of ordinary wildcard builds.
`adapter_test` applies a fixture aspect that shares the production adapter's
source classification, input collection, flags, tools, and environment. It
checks fixture targets, not policies in unrelated real dependency crates. The
production aspect checks all authored in-tree sources. The test aspect compares
exact normalized errors with optional `.out` goldens and publishes a marker
only when the comparison succeeds. Its `targets` attribute is distinct
from the production aspect's dependency edges, so both configurations can be
used without applying two custom-lint actions to the same provider.

- `passing` stages authored sources beside an unannotated generated schema.
  Its root, authored include, and attribute-macro input must remain checked;
  the schema and function-macro-generated helper must be excluded. Compilation
  also requires an aliased dependency, a feature flag, a direct environment
  entry, and an environment file.
- `binary` checks binary providers and propagation through Rust dependencies.
- `macros` checks proc-macro providers and execution-configured dependencies.
- `passing_test` checks wrapped library-test providers with staged sources.
- `shared` exercises `TestCrateInfo`, rather than `CrateInfo`.
- `missing` diagnoses both inherited-only policy and original real CXX input.
- `missing_test` diagnoses a module active only in standalone test mode.
- `inherited_tests` passes as a library; `inherited_tests_test` diagnoses its
  unannotated test module, using source ownership inherited from the library.
- `staged_missing` proves that missing policies in staged authored inline,
  external-file, and included modules still produce exactly one diagnostic
  each, without diagnosing the generated schema.

These fixtures supplement the syntactic contract and loader-failure cases in
[`../explicit_unsafe`](../explicit_unsafe/README.md). They are not a migration of
runtime crates or a claim of cross-platform coverage.
