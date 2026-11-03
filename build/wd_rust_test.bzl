load("@bazel_skylib//rules:write_file.bzl", "write_file")
load("@rules_rust//rust:defs.bzl", "rust_clippy_aspect", "rust_test", "rustfmt_aspect")

def split_rust_test_srcs(srcs):
    """Partitions a crate's sources into (production sources, unit test sources).

    A module's unit tests live beside it in <module>-test.rs, which the module declares as
    `#[cfg(test)] #[path = "<module>-test.rs"] mod tests;`. Only the crate's test target compiles
    those files, so they are inputs of the test and not of the crate itself. A rust_test built
    from a `crate` rejects `srcs`, so the test takes them as `compile_data`.
    """
    test_srcs = [src for src in srcs if src.endswith("-test.rs")]
    return [src for src in srcs if src not in test_srcs], test_srcs

def wd_rust_test(
        name,
        env = {},
        link_deps = [],
        tags = [],
        **kwargs):
    rust_test(
        name = name,
        env = {
            "RUST_BACKTRACE": "1",
            # Rust's test runner captures stderr by default, which makes debugging tests difficult.
            "RUST_TEST_NOCAPTURE": "1",
            # Rust tests in this repository are often heavyweight or rely on process-global state.
            "RUST_TEST_THREADS": "1",
        } | env,
        experimental_use_cc_common_link = 1,
        link_deps = link_deps + [
            "//build/deps:linkopts_default",
            "@@//deps:rust_runtime",
        ],
        malloc = "//src/workerd/server:malloc",
        # linkopts_default limits linker parallelism to avoid resource exhaustion.
        tags = tags + ["no-coverage", "cpu:2"],
        **kwargs
    )

def _rust_filtered_test_impl(ctx):
    # A test rule runs an executable of its own, so this one is the binary under the test's name,
    # as in kj_test: every filter shares the binary instead of linking one.
    binary = ctx.executable.binary
    executable = ctx.actions.declare_file(ctx.label.name)
    ctx.actions.symlink(output = executable, target_file = binary, is_executable = True)

    # The binary is `manual`, so a wildcard build skips it, and with it the checks that
    # --config=lint requests through these output groups. The filters carry them instead.
    output_groups = {
        group: getattr(ctx.attr.binary[OutputGroupInfo], group)
        for group in ["clippy_checks", "rustfmt_checks"]
        if hasattr(ctx.attr.binary[OutputGroupInfo], group)
    }
    return [
        DefaultInfo(executable = executable, runfiles = ctx.attr.binary[DefaultInfo].default_runfiles),
        # The binary's environment, RUST_TEST_THREADS=1 among it.
        ctx.attr.binary[RunEnvironmentInfo],
        OutputGroupInfo(**output_groups),
    ]

_rust_filtered_test = rule(
    implementation = _rust_filtered_test_impl,
    test = True,
    attrs = {
        "binary": attr.label(
            executable = True,
            cfg = "target",
            mandatory = True,
            aspects = [rust_clippy_aspect, rustfmt_aspect],
        ),
    },
)

def wd_rust_integration_tests(
        name,
        srcs,
        crate,
        deps = [],
        size = "medium",
        tags = [],
        **kwargs):
    """Cargo-style integration tests, in one test binary that runs as a test target per file.

    Each file (tests/foo.rs) is a module of a test crate, <name>_bin, that uses the crate under
    test through its public API. The crate root is generated from srcs, so every file is in it.
    The binary links once, and <name>_foo runs only tests::foo's tests, with a process and a
    timeout of its own; within it, the tests still run serially.

    Each file starts with `#![cfg(test)]`. Clippy's allow-*-in-tests settings (clippy.toml) exempt
    only #[test] functions and #[cfg(test)] items, so without it the file's helper functions are
    held to clippy::unwrap_used.

    A test target filters by module, and libtest ORs filters, so `--test_arg=<name>` does not
    narrow it. To run single tests, run the binary: `bazel run <name>_bin -- tests::foo::bar`.

    Args:
        name: prefix of the generated targets; the test of tests/foo.rs is <name>_foo.
        srcs: the test files, all in one directory.
        crate: the crate under test, a dependency of the test binary.
        deps: further dependencies of the test binary.
        size: Bazel test size of each test target.
        tags: tags of each test target.
        **kwargs: passed to the wd_rust_test of the binary.

    Returns:
        The labels of the generated tests.
    """
    modules = [src.split("/")[-1].removesuffix(".rs") for src in srcs]
    root = name + "_root.rs"

    # A crate root resolves modules as a mod.rs file does, so `mod foo;` inside `mod tests`
    # finds tests/foo.rs.
    write_file(
        name = name + "_root",
        out = root,
        content = ["mod tests {"] + ["    mod %s;" % module for module in modules] + ["}", ""],
        testonly = True,
    )
    wd_rust_test(
        name = name + "_bin",
        srcs = [root] + srcs,
        crate_root = root,
        crate_name = (name + "_bin").replace("-", "_"),
        deps = [crate] + deps,
        size = size,
        # Run through the filters below, not on its own.
        tags = tags + ["manual"],
        **kwargs
    )
    tests = []
    for module in modules:
        test_name = "%s_%s" % (name, module)
        _rust_filtered_test(
            name = test_name,
            binary = ":%s_bin" % name,
            # libtest matches substrings: the prefix and the trailing :: keep tests::gc:: from
            # matching tests::collections_gc, and tests::jsg_oneof:: from jsg_oneof_variants.
            args = ["tests::%s::" % module],
            size = size,
            # As wd_rust_test's.
            tags = tags + ["no-coverage", "cpu:2"],
        )
        tests.append(":" + test_name)
    return tests
