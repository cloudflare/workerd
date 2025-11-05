"""wd_cc_capnp_library definition"""

load("@capnp-cpp//src/capnp:cc_capnp_library.bzl", "cc_capnp_library")

def wd_cc_capnp_library(target_compatible_with = None, **kwargs):
    """Wrapper for cc_capnp_library that sets common attributes
    """
    if target_compatible_with == None:
        target_compatible_with = select({
            "@//build/config:no_build": ["@platforms//:incompatible"],
            "//conditions:default": [],
        })

    cc_capnp_library(
        target_compatible_with = target_compatible_with,
        src_prefix = "src",
        strip_include_prefix = "/src",
        **kwargs
    )
