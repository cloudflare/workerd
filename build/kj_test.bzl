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
            "//build/deps:linkopts_default",
        ] + deps,
        linkstatic = select({
            "@platforms//os:linux": 0,
            "//conditions:default": 1,
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
