load("@rules_rust//rust:defs.bzl", "rust_test")

def wd_rust_test(
        name,
        env = {},
        link_deps = [],
        tags = [],
        target_compatible_with = [],
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
        link_deps = link_deps + ["//build/deps:linkopts_default"],
        malloc = "//src/workerd/server:malloc",
        # linkopts_default limits linker parallelism to avoid resource exhaustion.
        tags = tags + ["no-coverage", "cpu:4"],
        target_compatible_with = select({
            "@//build/config:no_build": ["@platforms//:incompatible"],
            "//conditions:default": [],
        }) + target_compatible_with,
        **kwargs
    )
