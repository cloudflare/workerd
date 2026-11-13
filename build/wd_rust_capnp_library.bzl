"""wd_rust_capnp_library definition"""

load("@capnp-cpp//src/capnp:rust_capnp_library.bzl", "rust_capnp_library")

def wd_rust_capnp_library(**kwargs):
    """Wrapper for rust_capnp_library that sets common attributes
    """
    rust_capnp_library(
        src_prefix = "src",
        capnp_crate = "@crates_vendor//:capnp",
        capnpc_plugin = "@crates_vendor//:capnpc__capnpc-rust",
        **kwargs
    )
