"""CXX Bridge rules."""

load("@bazel_skylib//rules:run_binary.bzl", "run_binary")
load("//:build/wd_cc_library.bzl", "wd_cc_library")

def rust_cxx_bridge(name, src, deps = [], hdrs = [], linkstatic = True, include_prefix = None, strip_include_prefix = None, **kwargs):
    """A macro defining a cxx bridge library

    Args:
        name (string): The name of the new target
        src (string): The rust source file to generate a bridge for
        deps (list, optional): A list of dependencies for the underlying cc_library. Defaults to [].
        **kwargs: Common arguments to pass through to underlying rules.
    """
    native.alias(
        name = "%s/header" % name,
        actual = src + ".h",
        **kwargs
    )

    native.alias(
        name = "%s/source" % name,
        actual = src + ".cc",
        **kwargs
    )

    run_binary(
        name = "%s/generated" % name,
        srcs = [src],
        outs = [
            src + ".h",
            src + ".cc",
        ],
        args = [
            "$(execpath %s)" % src,
            "-o",
            "$(execpath %s.h)" % src,
            "-o",
            "$(execpath %s.cc)" % src,
        ],
        tool = "//src/rust/cxx:codegen",
        target_compatible_with = select({
            "@//build/config:no_build": ["@platforms//:incompatible"],
            "//conditions:default": [],
        }),
        **kwargs
    )

    wd_cc_library(
        name = name,
        srcs = [src + ".cc"],
        deps = deps + [":%s/include" % name],
        linkstatic = linkstatic,
        include_prefix = include_prefix,
        strip_include_prefix = strip_include_prefix,
        **kwargs
    )

    wd_cc_library(
        name = "%s/include" % name,
        hdrs = [src + ".h"] + hdrs,
        include_prefix = include_prefix,
        strip_include_prefix = strip_include_prefix,
        deps = deps,
        **kwargs
    )
