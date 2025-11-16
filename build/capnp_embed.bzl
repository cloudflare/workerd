"""capnp_embed definition"""

load("@capnp-cpp//src/capnp:cc_capnp_library.bzl", "capnp_provider")
load("@rules_cc//cc:cc_library.bzl", "cc_library")

def _capnp_embed_impl(ctx):
    return [
        capnp_provider(
            includes = [ctx.file.src.dirname],
            inputs = [ctx.file.src],
            src_prefix = "",
        ),
    ]

_capnp_embed = rule(
    attrs = {
        "src": attr.label(allow_single_file = True),
        "deps": attr.label_list(),
    },
    implementation = _capnp_embed_impl,
)

def capnp_embed(
        name,
        src,
        visibility = None,
        target_compatible_with = None,
        deps = []):
    """
    Bazel rule to include `src` in a Cap'n Proto search path for embedding.

    This is useful for including embedding the output of a `genrule` in a Cap'n Proto schema.
    The generated target should be included in `cc_capnp_library` `deps`.
    """
    if target_compatible_with == None:
        target_compatible_with = select({
            "@//build/config:no_build": ["@platforms//:incompatible"],
            "//conditions:default": [],
        })

    _capnp_embed(
        name = name + "_gen",
        src = src,
        visibility = visibility,
        target_compatible_with = target_compatible_with,
        deps = deps,
    )
    cc_library(
        name = name,
        target_compatible_with = target_compatible_with,
    )
