# Custom Rust lint fixtures

Each check has a dedicated subdirectory. The first collection is
[`explicit_unsafe/`](explicit_unsafe/README.md).

`runner.py` is a single-invocation comparator used by Bazel's `DylintFixture`
actions. Starlark owns fixture discovery, expected-output selection, compiler
arguments/environment, action inputs, and test-suite grouping. The comparator
does not maintain a case manifest or choose compiler configurations.

Loader-failure cases are independent Bazel targets grouped by `loader_test`.
They compile a valid policy fixture with an invalid library configuration and
must return nonzero.
