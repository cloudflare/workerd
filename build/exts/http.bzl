"""
A Bazel module extension that wraps the http_archive and http_file repository rules, in order
to allow the injection of a custom proxy URL.
"""

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive", "http_file")
load("//:build/http_proxy_config.bzl", "PROXY_URL")

def _http_ext_impl(ctx):
    for mod in ctx.modules:
        for archive in mod.tags.archive:
            _http_ext_delegate(http_archive, archive)

        for file in mod.tags.file:
            _http_ext_delegate(http_file, file)

def _http_ext_delegate(repo_rule, tag_struct):
    tag_dict = _struct_to_dict(tag_struct)
    tag_dict["url"] = PROXY_URL + tag_dict["url"]
    repo_rule(**tag_dict)

def _struct_to_dict(tag_struct):
    new_dict = {}
    for k in dir(tag_struct):
        new_dict[k] = getattr(tag_struct, k)
    return new_dict

# The following content is taken from @bazel_tools//tools/build_defs/repo:http.bzl, with
# documentation stripped to save space. See <https://bazel.build/rules/lib/repo/http> for docs.

_http_archive_attrs = tag_class(attrs = {
    "name": attr.string(),
    "url": attr.string(),
    "urls": attr.string_list(),
    "sha256": attr.string(),
    "integrity": attr.string(),
    "netrc": attr.string(),
    "auth_patterns": attr.string_dict(),
    "canonical_id": attr.string(),
    "strip_prefix": attr.string(),
    "add_prefix": attr.string(
        default = "",
    ),
    "files": attr.string_keyed_label_dict(),
    "type": attr.string(),
    "patches": attr.label_list(
        default = [],
    ),
    "remote_file_urls": attr.string_list_dict(
        default = {},
    ),
    "remote_file_integrity": attr.string_dict(
        default = {},
    ),
    "remote_module_file_urls": attr.string_list(
        default = [],
    ),
    "remote_module_file_integrity": attr.string(
        default = "",
    ),
    "remote_patches": attr.string_dict(
        default = {},
    ),
    "remote_patch_strip": attr.int(
        default = 0,
    ),
    "patch_tool": attr.string(
        default = "",
    ),
    "patch_args": attr.string_list(
        default = [],
    ),
    "patch_strip": attr.int(
        default = 0,
    ),
    "patch_cmds": attr.string_list(
        default = [],
    ),
    "patch_cmds_win": attr.string_list(
        default = [],
    ),
    "build_file": attr.label(
        allow_single_file = True,
    ),
    "build_file_content": attr.string(),
    "workspace_file": attr.label(),
    "workspace_file_content": attr.string(),
})

_http_file_attrs = tag_class(attrs = {
    "name": attr.string(),
    "executable": attr.bool(),
    "downloaded_file_path": attr.string(
        default = "downloaded",
    ),
    "sha256": attr.string(),
    "integrity": attr.string(),
    "canonical_id": attr.string(),
    "url": attr.string(),
    "urls": attr.string_list(),
    "netrc": attr.string(),
    "auth_patterns": attr.string_dict(),
})

# Extension definition

http = module_extension(
    implementation = _http_ext_impl,
    tag_classes = {
        "archive": _http_archive_attrs,
        "file": _http_file_attrs,
    },
)
