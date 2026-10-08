load("@rules_rust//rust:defs.bzl", "rust_test")

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
        link_deps = link_deps + [
            "//build/deps:linkopts_default",
            "@@//deps:rust_runtime",
        ],
        malloc = "//src/workerd/server:malloc",
        # linkopts_default limits linker parallelism to avoid resource exhaustion.
        tags = tags + ["no-coverage", "cpu:2"],
        target_compatible_with = select({
            "@//build/config:no_build": ["@platforms//:incompatible"],
            "//conditions:default": [],
        }) + target_compatible_with,
        **kwargs
    )
