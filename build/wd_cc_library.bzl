"""wd_cc_library definition"""

load("@rules_cc//cc:cc_library.bzl", "cc_library")

def wd_cc_library(strip_include_prefix = "/src", copts = [], **kwargs):
    """Wrapper for cc_library that sets common attributes
    """
    cc_library(
        target_compatible_with = select({
            "@//build/config:no_build": ["@platforms//:incompatible"],
            "//conditions:default": [],
        }),
        # global CPU-specific options are inconvenient to specify with bazel – just set the options we
        # need for CRC32C used in in this target for now.
        # TODO(cleanup): Come up with a better approach to specify these options for all C/C++
        # targets - it appears to be impossible to set arch-specific copts without a custom
        # toolchain or manually setting a config from the CLI interface (which we can do in CI, but
        # can't easily do for local builds).
        copts = copts + select({
            "@platforms//cpu:aarch64": [
                "-mcrc",
            ],
            "@platforms//cpu:x86_64": [
                # Note that msse4.2 already includes CRC32C and SSSE3 extensions
                "-msse4.2",
            ],
        }),
        strip_include_prefix = strip_include_prefix,
        **kwargs
    )
