load("@bazel_skylib//rules:copy_file.bzl", "copy_file")
load("//:build/capnp_embed.bzl", "capnp_embed")

def _out_name(src):
    src = src.removesuffix("//file")
    src = src.removeprefix("@")
    return src.rsplit(":", 2)[-1].rsplit("/", 2)[-1]

def copy_to_generated(src, out_name = None, name = None):
    out_name = out_name or _out_name(src)
    if name == None:
        name = out_name + "@copy"
    copy_file(name = name, src = src, out = "generated/" + out_name)

def copy_and_capnp_embed(src, out_name = None):
    out_name = out_name or _out_name(src)
    copy_to_generated(src, out_name = out_name)
    file = "generated/" + out_name
    capnp_embed(
        name = out_name + "@capnp",
        src = file,
        deps = [out_name + "@copy"],
    )
