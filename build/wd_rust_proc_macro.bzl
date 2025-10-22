load("@rules_rust//rust:defs.bzl", "rust_proc_macro", "rust_test")

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
    srcs = native.glob(["**/*.rs"])
    crate_name = name.replace("-", "_")

    rust_proc_macro(
        name = name,
        crate_name = crate_name,
        srcs = srcs,
        deps = deps + ["@workerd//deps/rust:runtime"],
        visibility = visibility,
        data = data,
        target_compatible_with = select({
            "@//build/config:no_build": ["@platforms//:incompatible"],
            "//conditions:default": [],
        }),
    )

    rust_test(
        name = name + "_test",
        crate = ":" + name,
        env = {
            "RUST_BACKTRACE": "1",
            # rust test runner captures stderr by default, which makes debugging tests very hard
            "RUST_TEST_NOCAPTURE": "1",
            # our tests are usually very heavy and do not support concurrent invocation
            "RUST_TEST_THREADS": "1",
        } | test_env,
        tags = test_tags,
        deps = test_deps,
        experimental_use_cc_common_link = select({
            "@platforms//os:windows": 0,
            "//conditions:default": 1,
        }),
    )
