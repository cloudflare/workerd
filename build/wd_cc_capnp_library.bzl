"""wd_cc_capnp_library definition"""

load("@capnp-cpp//src/capnp:cc_capnp_library.bzl", "cc_capnp_library")

def wd_cc_capnp_library(**kwargs):
    """Wrapper for cc_capnp_library that sets common attributes
    """
    cc_capnp_library(
        src_prefix = "src",
        strip_include_prefix = "/src",
        **kwargs
    )
