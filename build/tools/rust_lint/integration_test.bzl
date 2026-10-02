"""Exercise the production Dylint adapter against real Rust crate providers."""

load(":rust_lint.bzl", "RUST_LINT_ASPECT_ATTRS", "RUST_LINT_TOOLCHAINS", "WorkerdRustLintInfo", "rust_lint_aspect_impl")

_FIXTURE_PACKAGE = Label("//tools/rust-lints/tests/integration:adapter_test").package

def _fixture_aspect_impl(target, ctx):
    # Golden outputs describe fixture policies, not unmigrated dependency crates.
    return rust_lint_aspect_impl(target, ctx, fixture = True, check = target.label.package == _FIXTURE_PACKAGE)

_fixture_aspect = aspect(
    implementation = _fixture_aspect_impl,
    attr_aspects = ["deps", "proc_macro_deps", "crate", "link_deps"],
    attrs = dict(RUST_LINT_ASPECT_ATTRS, _fixture_runner = attr.label(default = "//tools/rust-lints:fixture_runner", executable = True, cfg = "exec")),
    fragments = ["cpp"],
    toolchains = RUST_LINT_TOOLCHAINS,
    provides = [WorkerdRustLintInfo],
)

def _integration_test_impl(ctx):
    for target in ctx.attr.targets:
        own_checks = [file for file in target[WorkerdRustLintInfo].checks.to_list() if file.owner == target.label]
        if not own_checks:
            fail("Integration fixture was not checked: " + str(target.label))
    for target in ctx.attr.exempt_targets:
        own_checks = [file for file in target[WorkerdRustLintInfo].checks.to_list() if file.owner == target.label]
        if own_checks:
            fail("Test-only integration fixture was checked: " + str(target.label))
    checks = depset(transitive = [target[WorkerdRustLintInfo].checks for target in ctx.attr.targets + ctx.attr.exempt_targets]).to_list()
    executable = ctx.actions.declare_file(ctx.label.name + ".sh")
    ctx.actions.write(executable, "#!/usr/bin/env bash\nset -euo pipefail\n" + "\n".join([
        'test -f "$TEST_SRCDIR/$TEST_WORKSPACE/' + file.short_path + '"'
        for file in checks
    ]) + "\n", is_executable = True)
    return [DefaultInfo(executable = executable, runfiles = ctx.runfiles(files = checks))]

rust_lint_integration_test = rule(
    implementation = _integration_test_impl,
    test = True,
    attrs = {
        "targets": attr.label_list(aspects = [_fixture_aspect], mandatory = True),
        "exempt_targets": attr.label_list(aspects = [_fixture_aspect]),
    },
)
