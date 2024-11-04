load("@rules_rust//rust:defs.bzl", "rust_binary", "rust_test")
load("@workerd//:build/wd_rust_crate.bzl", "rust_cxx_bridge")

def wd_rust_binary(
        name,
        deps = [],
        proc_macro_deps = [],
        data = [],
        rustc_env = {},
        visibility = None,
        tags = [],
        cxx_bridge_src = None,
        cxx_bridge_deps = []):
    """Define rust binary.

    Args:
        name: crate name.
        deps: crate dependencies: rust crates or c/c++ libraries.
        visibility: crate visibility.
        data: additional data files.
        proc_macro_deps: proc_macro dependencies.
        rustc_env: additional rustc environment variables,
        tags: rule tags
    """
    srcs = native.glob(["**/*.rs"])
    crate_name = name.replace("-", "_")

    if cxx_bridge_src:
        hdrs = native.glob(["**/*.h"], allow_empty = True)

        rust_cxx_bridge(
            name = name + "@cxx",
            src = cxx_bridge_src,
            hdrs = hdrs,
            include_prefix = "workerd/rust/" + name,
            strip_include_prefix = "",
            # Not applying visibility here – if you import the cxxbridge header, you will likely
            # also need the rust library itself to avoid linker errors.
            deps = cxx_bridge_deps + [
                "@crates_vendor//:cxx",
                "//src/rust/cxx-integration:cxx-include",
            ],
        )

        deps.append("@crates_vendor//:cxx")
        deps.append(name + "@cxx")

    rust_binary(
        name = name,
        crate_name = crate_name,
        srcs = srcs,
        rustc_env = rustc_env,
        deps = deps,
        visibility = visibility,
        data = data,
        proc_macro_deps = proc_macro_deps,
        experimental_use_cc_common_link = select({
            "@platforms//os:windows": 0,
            "//conditions:default": 1,
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
        },
        experimental_use_cc_common_link = select({
            "@platforms//os:windows": 0,
            "//conditions:default": 1,
        }),
    )
