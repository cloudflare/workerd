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

    native.cc_binary(
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
        linkopts = select({
            "@//:use_dead_strip": ["-Wl,-dead_strip", "-Wl,-no_exported_symbols"],
            "//conditions:default": [""],
        }),
        data = data,
        tags = tags,
        **kwargs
    )
    sh_test(
        name = test_name,
        size = size,
        srcs = ["//build/fixtures:kj_test.sh"],
        data = [binary_name],
        args = ["$(location " + binary_name + ")"],
    )
    sh_test(
        name = test_name + "@all-autogates-enabled",
        size = size,
        env = {"WORKERD_ALL_AUTOGATES": "1"},
        srcs = ["//build/fixtures:kj_test.sh"],
        data = [binary_name],
        args = ["$(location " + binary_name + ")"],
    )
