load("@rules_rust//rust:defs.bzl", "rust_library", "rust_test")

def rust_cxx_include(name, visibility = [], include_prefix = None):
    native.genrule(
        name = "%s/generated" % name,
        outs = ["cxx.h"],
        cmd = "$(location @cxxbridge-cmd//:cxxbridge-cmd) --header > \"$@\"",
        tools = ["@cxxbridge-cmd//:cxxbridge-cmd"],
    )

    native.cc_library(
        name = name,
        hdrs = ["cxx.h"],
        include_prefix = include_prefix,
        visibility = visibility,
    )

def rust_cxx_bridge(
        name,
        src,
        hdrs = [],
        deps = [],
        visibility = [],
        strip_include_prefix = None,
        include_prefix = None):
    native.genrule(
        name = "%s/generated" % name,
        srcs = [src],
        outs = [
            src + ".h",
            src + ".cc",
        ],
        cmd = "$(location @cxxbridge-cmd//:cxxbridge-cmd) $(location %s) -o $(location %s.h) -o $(location %s.cc)" % (src, src, src),
        tools = ["@cxxbridge-cmd//:cxxbridge-cmd"],
    )

    native.cc_library(
        name = name,
        srcs = [src + ".cc"],
        hdrs = [src + ".h"] + hdrs,
        strip_include_prefix = strip_include_prefix,
        include_prefix = include_prefix,
        linkstatic = True,
        deps = deps,
        visibility = visibility,
    )

def wd_rust_crate(
        name,
        cxx_bridge_src = None,
        deps = [],
        proc_macro_deps = [],
        data = [],
        test_env = {},
        test_tags = [],
        test_deps = [],
        test_proc_macro_deps = [],
        cxx_bridge_deps = [],
        visibility = None):
    """Define rust crate.

    Args:
        name: crate name.
        cxx_bridge_src: (optional) .rs source file with cxx ffi bridge definition. The rule will
            generation additional<name>@cxx c++ library with cxx bindings if this is set.
        deps: crate dependencies: rust crates or c/c++ libraries.
        visibility: crate visibility.
        data: additional data files.
        proc_macro_deps: proc_macro dependencies.
        rustc_env: rustc environment variables,
        test_env: additional test environment variable.
        test_tags: additional test tags.
        test_deps: test-only dependencies.
        test_proc_macro_deps: test-only proc_macro dependencies.
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

    crate_features = []

    rust_library(
        name = name,
        crate_name = crate_name,
        srcs = srcs,
        deps = deps,
        visibility = visibility,
        data = data,
        proc_macro_deps = proc_macro_deps,
        crate_features = crate_features,
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
        crate_features = crate_features,
        deps = test_deps,
        proc_macro_deps = test_proc_macro_deps,
        experimental_use_cc_common_link = select({
            "@platforms//os:windows": 0,
            "//conditions:default": 1,
        }),
    )
