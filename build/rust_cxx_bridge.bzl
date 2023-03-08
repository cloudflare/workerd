load("@rules_cc//cc:defs.bzl", "cc_library")

def rust_cxx_include(name, deps = [], visibility = [], include_prefix = None, target_compatible_with = None):
    native.genrule(
        name = "%s/generated" % name,
        outs = ["cxx.h"],
        cmd = "$(location @cxxbridge_vendor//:cxxbridge-cmd__cxxbridge) --header > \"$@\"",
        tools = select({
            # "@cxxbridge_vendor//:cxxbridge-cmd__cxxbridge" cannot be found on Windows
            "@platforms//os:windows": [],
            "//conditions:default": ["@cxxbridge_vendor//:cxxbridge-cmd__cxxbridge"],
        }),
        target_compatible_with = select({
            "@platforms//os:windows": ["@platforms//:incompatible"],
            "//conditions:default": [],
        }),
    )
    cc_library(
        name = name,
        hdrs = ["cxx.h"],
        include_prefix = include_prefix,
        visibility = visibility,
        target_compatible_with = select({
            "@platforms//os:windows": ["@platforms//:incompatible"],
            "//conditions:default": [],
        }),
    )

def rust_cxx_bridge(name, src, deps = [], visibility = [], strip_include_prefix = None, include_prefix = None, target_compatible_with = None):
    native.genrule(
        name = "%s/generated" % name,
        srcs = [src],
        outs = [
            src + ".h",
            src + ".cc",
        ],
        cmd = "$(location @cxxbridge_vendor//:cxxbridge-cmd__cxxbridge) $(location %s) -o $(location %s.h) -o $(location %s.cc)" % (src, src, src),
        tools = select({
            # "@cxxbridge_vendor//:cxxbridge-cmd__cxxbridge" cannot be found on Windows
            "@platforms//os:windows": [],
            "//conditions:default": ["@cxxbridge_vendor//:cxxbridge-cmd__cxxbridge"],
        }),
        target_compatible_with = select({
            "@platforms//os:windows": ["@platforms//:incompatible"],
            "//conditions:default": [],
        }),
    )

    cc_library(
        name = name,
        srcs = [src + ".cc"],
        deps = deps + [":%s/include" % name],
        visibility = visibility,
    )

    cc_library(
        name = "%s/include" % name,
        hdrs = [src + ".h"],
        strip_include_prefix = strip_include_prefix,
        include_prefix = include_prefix,
        visibility = visibility,
        target_compatible_with = select({
            "@platforms//os:windows": ["@platforms//:incompatible"],
            "//conditions:default": [],
        }),
    )
