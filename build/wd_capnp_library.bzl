load("//:build/wd_cc_capnp_library.bzl", "wd_cc_capnp_library")
load("//:build/wd_rust_capnp_library.bzl", "wd_rust_capnp_library")

def wd_capnp_library(
        src,
        deps = [],
        tags = [],
        target_compatible_with = None,
        visibility = ["//visibility:public"]):
    """Generates capnp library for multiple languages.

    For a given file-name.capnp it will produce:
    - `file-name_capnp` c++ library
    - `file-name_capnp_rust` rust library
    """
    base_name = src.removesuffix(".capnp")

    wd_cc_capnp_library(
        name = base_name + "_capnp",
        visibility = visibility,
        deps = deps,
        srcs = [src],
        tags = ["manual"] + tags,
        target_compatible_with = target_compatible_with,
    )

    rust_deps = [dep + "_rust" if dep.endswith("_capnp") else dep for dep in deps]

    wd_rust_capnp_library(
        name = base_name + "_capnp_rust",
        crate_name = base_name.replace("-", "_") + "_capnp",
        visibility = visibility,
        deps = rust_deps,
        srcs = [src],
        tags = ["manual"] + tags,
        target_compatible_with = target_compatible_with,
    )
