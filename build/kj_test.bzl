load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_shell//shell:sh_test.bzl", "sh_test")

def kj_test(
        src,
        data = [],
        deps = [],
        tags = [],
        size = "medium",
        **kwargs):
    test_name = src.removesuffix(".c++")
    binary_name = test_name + "_binary"
    cc_binary(
        name = binary_name,
        testonly = True,
        srcs = [src],
        deps = [
            "@capnp-cpp//src/kj:kj-test",
        ] + deps,
        linkstatic = select({
            "@platforms//os:linux": 0,
            "//conditions:default": 1,
        }),
        # For test binaries, reduce thinLTO optimizations and inlining to speed up linking. This
        # only has an effect if thinLTO is enabled. Also apply dead_strip on macOS to manage binary
        # sizes.
        linkopts = select({
            "@platforms//os:linux": ["-Wl,--lto-O1", "-Wl,-mllvm,-import-instr-limit=5"],
            "@//:use_dead_strip": ["-Wl,-dead_strip", "-Wl,-no_exported_symbols"],
            "//conditions:default": [""],
        }),
        data = data,
        tags = tags,
        target_compatible_with = select({
            "@//build/config:no_build": ["@platforms//:incompatible"],
            "//conditions:default": [],
        }),
        **kwargs
    )

    pkg = native.package_name()
    cross_alias = binary_name + "_cross"
    native.alias(
        name = cross_alias,
        actual = select({
            "@//build/config:prebuilt_binaries_arm64": "@//:bin.arm64/tmp/workerd/{}/{}.aarch64-linux-gnu".format(pkg, binary_name),
            "//conditions:default": binary_name,
        }),
    )

    sh_test(
        name = test_name + "@",
        srcs = ["//build/fixtures:kj_test.sh"],
        args = ["$(location {})".format(cross_alias)],
        data = data + [cross_alias],
        tags = tags,
        size = size,
    )

    sh_test(
        name = test_name + "@all-autogates",
        srcs = ["//build/fixtures:kj_test.sh"],
        args = ["$(location {})".format(cross_alias)],
        data = data + [cross_alias],
        env = {"WORKERD_ALL_AUTOGATES": "1"},
        # Tag with no-coverage to reduce coverage CI time
        tags = tags + ["no-coverage"],
        size = size,
    )
