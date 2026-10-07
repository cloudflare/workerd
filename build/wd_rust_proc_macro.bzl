load("@rules_rust//rust:defs.bzl", "rust_proc_macro", "rust_test")
load("//:build/wd_rust_test.bzl", "split_rust_test_srcs")

def wd_rust_proc_macro(
        name,
        deps = [],
        data = [],
        test_env = {},
        test_tags = [],
        test_deps = [],
        visibility = None):
    """Define rust procedural macro crate.

    Args:
        name: crate name.
        deps: crate dependencies: rust crates (typically includes proc-macro2, quote, syn).
        data: additional data files.
        test_env: additional test environment variables.
        test_tags: additional test tags.
        test_deps: test-only dependencies.
        visibility: crate visibility.
    """
    srcs, test_srcs = split_rust_test_srcs(native.glob(["**/*.rs"]))
    crate_name = name.replace("-", "_")

    rust_proc_macro(
        name = name,
        crate_name = crate_name,
        srcs = srcs,
        deps = deps,
        visibility = visibility,
        data = data,
        lint_config = "@workerd//build/rust:lints",
        target_compatible_with = select({
            "@//build/config:no_build": ["@platforms//:incompatible"],
            "//conditions:default": [],
        }),
    )

    rust_test(
        name = name + "_test",
        compile_data = test_srcs,
        crate = ":" + name,
        env = {
            "RUST_BACKTRACE": "1",
            # rust test runner captures stderr by default, which makes debugging tests very hard
            "RUST_TEST_NOCAPTURE": "1",
            # our tests are usually very heavy and do not support concurrent invocation
            "RUST_TEST_THREADS": "1",
        } | test_env,
        experimental_use_cc_common_link = 1,
        # Tag with cpu:2 since this target depends on linkopts_default.
        tags = test_tags + ["no-coverage", "cpu:2"],
        deps = test_deps,
        link_deps = ["@@//deps:rust_runtime", "@workerd//build/deps:linkopts_default"],
        target_compatible_with = select({
            "@//build/config:no_build": ["@platforms//:incompatible"],
            "//conditions:default": [],
        }),
    )
