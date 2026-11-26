load("@rules_rust//rust:defs.bzl", "rust_clippy_aspect", "rust_test", "rustfmt_aspect")
load("//:build/test_attrs.bzl", "test_attrs")

def split_rust_test_srcs(srcs):
    """Partitions a crate's sources into (production sources, unit test sources).

    A module's unit tests live beside it in <module>-test.rs, which the module declares as
    `#[cfg(test)] #[path = "<module>-test.rs"] mod tests;`. Only the crate's test target compiles
    those files, so they are inputs of the test and not of the crate itself. A rust_test built
    from a `crate` rejects `srcs`, so the test takes them as `compile_data`.
    """
    test_srcs = [src for src in srcs if src.endswith("-test.rs")]
    return [src for src in srcs if src not in test_srcs], test_srcs

def _sharded_rust_test_impl(ctx):
    test = ctx.attr.test
    sharder = ctx.executable._sharder

    # A test's executable must be its own output.
    name = ctx.label.name
    if sharder.extension:
        name += "." + sharder.extension
    executable = ctx.actions.declare_file(name)
    ctx.actions.symlink(output = executable, target_file = sharder, is_executable = True)

    runfiles = ctx.runfiles(files = [ctx.executable.test])
    runfiles = runfiles.merge_all([
        test[DefaultInfo].default_runfiles,
        ctx.attr._sharder[DefaultInfo].default_runfiles,
    ])
    rlocation = ctx.expand_location("$(rlocationpath {})".format(test.label), [test])

    # The lint aspects that --config=lint applies are not run on targets that already have their
    # output groups, so this one stands for the test it runs.
    output_groups = test[OutputGroupInfo] if OutputGroupInfo in test else struct()
    lint_checks = {
        group: getattr(output_groups, group)
        for group in ["clippy_checks", "rustfmt_checks"]
        if hasattr(output_groups, group)
    }
    return [
        DefaultInfo(executable = executable, runfiles = runfiles),
        RunEnvironmentInfo(environment = ctx.attr.env | {"WD_SHARDED_RUST_TEST": rlocation}),
        OutputGroupInfo(**lint_checks),
    ]

_sharded_rust_test = rule(
    implementation = _sharded_rust_test_impl,
    test = True,
    attrs = {
        # The rust_test to shard. It is a manual target, which `bazel build //...` does not lint,
        # so its lint checks become this target's.
        "test": attr.label(
            mandatory = True,
            executable = True,
            cfg = "target",
            aspects = [rust_clippy_aspect, rustfmt_aspect],
        ),
        "env": attr.string_dict(),
        "_sharder": attr.label(
            default = "//build/rust/test_sharder:sharder",
            executable = True,
            cfg = "target",
        ),
    },
)

def wd_rust_test(
        name,
        env = {},
        link_deps = [],
        tags = [],
        shard_count = None,
        size = "medium",
        target_compatible_with = None,
        exec_properties = {},
        **kwargs):
    """Defines a Rust test.

    Args:
        name: the test's name.
        env: additional environment variables of the test.
        link_deps: C/C++ libraries to link with the test.
        tags: the test's tags.
        shard_count: the number of Bazel test shards to split the test cases between, each run
            in its own process. The cases of one shard still run serially. Shard a test whose
            cases together take too long but can run in separate processes. The rust_test is then
            `<name>@unsharded`, run through //build/rust/test_sharder.
        size: the test's size.
        target_compatible_with: platform constraints of the test.
        exec_properties: the test's own exec_properties; see test_attrs.
        **kwargs: further rust_test attributes.
    """
    env = {
        "RUST_BACKTRACE": "1",
        # Rust's test runner captures stderr by default, which makes debugging tests difficult.
        "RUST_TEST_NOCAPTURE": "1",
        # Rust tests in this repository are often heavyweight or rely on process-global state.
        "RUST_TEST_THREADS": "1",
    } | env
    attrs = test_attrs(tags + ["no-coverage"], size, target_compatible_with, exec_properties)
    test_name = name
    if shard_count:
        test_name = name + "@unsharded"
        _sharded_rust_test(
            name = name,
            test = ":" + test_name,
            env = env,
            shard_count = shard_count,
            size = size,
            **attrs
        )

        # Run through the sharded test only, not on its own as well.
        attrs = attrs | {"tags": attrs["tags"] + ["manual"]}

    rust_test(
        name = test_name,
        env = env,
        experimental_use_cc_common_link = 1,
        # linkopts_default limits linker parallelism to avoid resource exhaustion.
        link_deps = link_deps + [
            "//build/deps:linkopts_default",
            "@@//deps:rust_runtime",
        ],
        malloc = "//src/workerd/server:malloc",
        size = size,
        **(kwargs | attrs)
    )
