# Explicit unsafe-policy fixtures

```sh
bazel test //tools/rust-lints:explicit_unsafe_policy_test \
  --config=dylint --test_output=errors
```

`explicit_unsafe_tests` in `build/tools/rust_lint/fixture.bzl` discovers every
root `.rs` file here and its optional matching `<original_name>.out`. A fixture
without an output file must compile without errors. A fixture with an output
file must fail, producing exactly those normalized errors. There is no shared
case manifest. Supporting modules and include fragments live under `modules/`
and are not standalone compilation roots.

Bazel creates separate library and test-mode actions for each root. The
`inactive_test` root is library-only and `active_test` is test-only. Every
invocation enables `policy_active`; inactive conditional policies use an
explicitly false predicate. New ordinary fixtures are discovered without
editing a target list.

Golden output contains sorted error messages with filename and error code.
Sandbox prefixes, pass traversal order, final abort summaries, and unrelated
warnings are excluded. Missing or duplicate custom diagnostics, incorrect
external declaration files, unexpected errors, and incorrect exit status fail
the build action. Do not create empty `.out` files for passing fixtures.

- `pass.rs`: nested/inline/file modules, outer and inner policies, `mod.rs`,
  `#[path]`, active cfg_attr, inactive parents, actual CXX input, generated
  helpers, and exempt test modules.
- `missing.rs`: absent root/child/file/CXX/include policies, fake comments,
  inactive policies, warn/expect, deny(warnings), function-only declarations,
  and a different unsafe-related lint.
- `forbid_parent.rs`: inherited forbid does not satisfy local declarations.
- `policies.rs`: forbid and multi-lint attributes are accepted.
- `conditional_missing.rs`: an inactive cfg_attr policy does not count.
- `active_test.rs` / `inactive_test.rs`: test-gated modules need no policy in either mode.
- `test_exempt.rs`: nested, out-of-line, inner-file-cfg, conditional, and CXX test
  modules are exempt, including descendants in other files.
- `test_exempt_production.rs`: inner test cfg attributes do not exempt sibling
  production modules or their crate root.
- `mixed_test.rs`: code enabled by either `test` or a production predicate still
  requires a policy in both compilation modes.
- `denied_unsafe.rs` / `allowed_unsafe.rs`: rustc enforces the actual policy.
- `suppressed.rs`: command-line forbid rejects source-level custom-lint allow.

A loaded supporting file's inner cfg disables its descendants. Generated
helpers include a macro-spliced caller identifier: its identifier span alone
does not prove that a module is authored. Include fragments create no extra root
boundary, but module declarations within them are checked. CXX cases use the
repository's real proc macro rather than a stand-in expansion.

These are isolated compiler inputs, not runtime crates. Keep expected output in
sync with the source and [rule contract](../../README.md).
