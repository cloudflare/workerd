load("@bazel_skylib//rules:write_file.bzl", "write_file")
load("//:build/wd_cc_library.bzl", "wd_cc_library")

# Normalizes an embed name constructed from the embed path.
def normalize_embed_name(file_name):
    return file_name.replace(".", "_").replace("-", "_").replace("/", "_").upper()

# Converts file name or label into string of the corresponding embed name.
def wd_cc_embed_name(src):
    embed_filename = native.package_relative_label(src).name
    return normalize_embed_name(embed_filename)

# A high-performance embed mechanism built upon C23. Prefer this over capnp embeds unless the embed is needed within capnp definitions.
# name: A valid C variable name.
# src: The name of the file to be embedded.
# Generates a ${name} library target and a ${base_name}.embed.h header for accessing the variable name as a const char array.
def wd_cc_embed(name, src, base_name = "", is_text = False, **kwargs):
    embed_filename = native.package_relative_label(src).name
    embed_package = native.package_relative_label(src).package
    embed_name = normalize_embed_name(embed_filename)

    # Heuristically determine if file is intended to be used as text or binary.
    is_text = is_text or embed_filename.endswith(".txt") or embed_filename.endswith(".js") or embed_filename.endswith(".ts")
    pod_data_type = "unsigned char"
    data_type = "kj::ArrayPtr<const kj::byte>"
    if is_text:
        pod_data_type = "char"
        data_type = "kj::StringPtr"

    # Optionally construct output file names from embed file name
    if (base_name == ""):
        base_name = embed_filename.replace(".", "_").replace("-", "_")
    else:
        embed_name = normalize_embed_name(base_name)

    write_file(
        name = embed_filename + "@c",
        out = base_name + ".embed.c",
        content = ["""#include <stddef.h>
const {pod_data_type} {embed_name}_begin[] = {{
  #embed <{embed}>
}};
size_t {embed_name}_size = sizeof({embed_name}_begin);
""".format(embed_name = embed_name, embed = embed_filename, pod_data_type = pod_data_type)],
    )
    write_file(
        name = embed_filename + "@h",
        out = base_name + ".embed.h",
        content = ["""#pragma once
#include <kj/common.h>
#include <kj/string.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {{
#endif
extern const {pod_data_type} {embed_name}_begin[];
extern size_t {embed_name}_size;
#define {embed_name} ({data_type}({embed_name}_begin, {embed_name}_size))
#ifdef __cplusplus
}}
#endif""".format(embed_name = embed_name, data_type = data_type, pod_data_type = pod_data_type)],
    )

    wd_cc_library(
        name = name,
        srcs = [base_name + ".embed.c"],
        hdrs = [base_name + ".embed.h"],
        additional_compiler_inputs = [src],
        conlyopts = [
            # no need to have debug info for the embed
            "-g0",
            # C23 is needed for N3017 #embed. Clang also supports it in C++26 mode but it is not
            # officially part of that standard so far; no harm in using C on a per-file basis.
            "-std=c23",
            # Hack: We need to provide the path to the embed file directory here. We can access the
            # embed file easily by adding it to additional_compiler_inputs but can't easily get its
            # directory within a macro – provide both the path of the package and its output
            # directory to support both pre-existing and generated files. To support using the macro
            # with external repositories in the future, we may need to add the file's workspace_root
            # too.
            "--embed-dir=" + embed_package,
            "--embed-dir=$(GENDIR)/" + embed_package,
        ],
        deps = [
            "@capnp-cpp//src/kj",
        ],
        **kwargs
    )
