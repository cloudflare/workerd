"""wd_cc_binary definition"""

load("@rules_cc//cc:cc_binary.bzl", "cc_binary")

def wd_cc_binary(
        name,
        visibility = None,
        deps = [],
        target_compatible_with = [],
        **kwargs):
    """Wrapper for cc_binary that sets common attributes
    """
    cc_binary(
        name = name,
        target_compatible_with = select({
            "@//build/config:no_build": ["@platforms//:incompatible"],
            "//conditions:default": [],
        }) + target_compatible_with,
        visibility = visibility,
        deps = deps + ["//build/deps:linkopts_tool"],
        **kwargs
    )

    pkg = native.package_name().removeprefix("src/")
    cross_alias = name + "_cross"
    prebuilt_binary_name = name.removesuffix("_bin")
    native.alias(
        name = cross_alias,
        visibility = visibility,
        actual = select({
            "@//build/config:prebuilt_binaries_arm64": "@//:bin.arm64/tmp/{}/{}.aarch64-linux-gnu".format(pkg, prebuilt_binary_name),
            "//conditions:default": name,
        }),
        testonly = kwargs.get("testonly", False),
    )
