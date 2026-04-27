load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_rust//rust:defs.bzl", "rust_library", "rust_test", "rust_unpretty")

def rust_cxx_bridge(
        name,
        src,
        hdrs = [],
        deps = [],
        visibility = [],
        strip_include_prefix = None,
        include_prefix = None,
        tags = [],
        local_defines = [],
        features = []):
    native.genrule(
        name = "%s/generated" % name,
        srcs = [src],
        outs = [
            src + ".h",
            src + ".cc",
        ],
        cmd = "$(location @workerd-cxx//:codegen) $(location %s) -o $(location %s.h) -o $(location %s.cc)" % (src, src, src),
        tools = ["@workerd-cxx//:codegen"],
        target_compatible_with = select({
            "@//build/config:no_build": ["@platforms//:incompatible"],
            "//conditions:default": [],
        }),
    )
    cc_library(
        name = name,
        srcs = [src + ".cc"],
        hdrs = [src + ".h"] + hdrs,
        strip_include_prefix = strip_include_prefix,
        include_prefix = include_prefix,
        local_defines = local_defines,
        features = features,
        linkstatic = select({
            "@platforms//os:windows": True,
            "//conditions:default": False,
        }),
        deps = deps,
        visibility = visibility,
        tags = tags,
        target_compatible_with = select({
            "@//build/config:no_build": ["@platforms//:incompatible"],
            "//conditions:default": [],
        }),
    )

def wd_rust_crate(
        name,
        cxx_bridge_src = None,
        cxx_bridge_srcs = [],
        deps = [],
        proc_macro_deps = [],
        data = [],
        test_env = {},
        test_tags = [],
        test_deps = [],
        test_proc_macro_deps = [],
        test_size = "medium",
        cxx_bridge_deps = [],
        cxx_bridge_tags = [],
        cxx_bridge_local_defines = [],
        cxx_bridge_features = [],
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
        test_size: Bazel test size (default "medium"); affects test timeout. Use "large" for
            crates with many subtests, since RUST_TEST_THREADS=1 forces serial execution.
        cxx_bridge_deps: either a flat dependency list applied to every bridge source, or a dict of
            bridge source => dependency list.
    """
    srcs = native.glob(["**/*.rs"])
    crate_name = name.replace("-", "_")

    if cxx_bridge_src:
        cxx_bridge_srcs = cxx_bridge_srcs + [cxx_bridge_src]

    if type(cxx_bridge_deps) == "dict":
        invalid_bridge_srcs = [bridge_src for bridge_src in cxx_bridge_deps.keys() if bridge_src not in cxx_bridge_srcs]
        if invalid_bridge_srcs:
            fail("cxx_bridge_deps contains keys not present in cxx_bridge_srcs: %s" % sorted(invalid_bridge_srcs))
    else:
        cxx_bridge_deps = {
            bridge_src: cxx_bridge_deps + []
            for bridge_src in cxx_bridge_srcs
        }

    # Add cxx dependency if there are any cxx bridges
    if len(cxx_bridge_srcs) > 0:
        deps = deps + [
            "@workerd-cxx//kj-rs",
            "@workerd-cxx//:cxx",
        ]

    include_prefix = "workerd/" + native.package_name().removeprefix("src/")

    hdrs = native.glob(["**/*.h"], allow_empty = True)
    for bridge_src in cxx_bridge_srcs:
        rust_cxx_bridge(
            name = bridge_src + "@cxx",
            src = bridge_src,
            hdrs = hdrs,
            include_prefix = include_prefix,
            strip_include_prefix = "",
            # Not applying visibility here – if you import the cxxbridge header, you will likely
            # also need the rust library itself to avoid linker errors.
            deps = cxx_bridge_deps.get(bridge_src, []) + [
                "@workerd-cxx//kj-rs",
                "@workerd-cxx//:cxx",
            ],
            tags = cxx_bridge_tags,
            local_defines = cxx_bridge_local_defines,
            features = cxx_bridge_features,
        )

    for bridge_src in cxx_bridge_srcs:
        deps.append(bridge_src + "@cxx")

    crate_features = []

    rust_library(
        name = name,
        crate_name = crate_name,
        srcs = srcs,
        deps = deps + ["@workerd//deps/rust:runtime"],
        visibility = visibility,
        data = data,
        proc_macro_deps = proc_macro_deps,
        crate_features = crate_features,
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
        size = test_size,
        tags = test_tags + ["no-coverage"],
        crate_features = crate_features,
        deps = test_deps,
        proc_macro_deps = test_proc_macro_deps,
    )

    if len(proc_macro_deps) + len(cxx_bridge_srcs) > 0:
        rust_unpretty(
            name = name + "@expand",
            deps = [":" + name],
            tags = ["manual", "off-by-default"],
        )

    if len(test_proc_macro_deps) > 0:
        rust_unpretty(
            name = name + "_test@expand",
            deps = [":" + name + "_test"],
            tags = ["manual", "off-by-default"],
            testonly = True,
        )
