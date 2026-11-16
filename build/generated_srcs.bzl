"""Collects the generated sources of C++ and Rust targets, for editors.

--config=bazelflare-generated adds the `generated_srcs` output group of this aspect to a build
and downloads the files it holds, the ones an editor reads (Cap'n Proto and cxx bridge headers,
generated Rust, the `_virtual_includes` header trees), into bazel-bin. `just fetch-generated`
builds that output group alone, so it compiles and downloads nothing else.
"""

load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")
load("@rules_rust//rust:rust_common.bzl", "CrateInfo", "TestCrateInfo")

GeneratedSrcsInfo = provider(
    doc = "Generated source files of a target and its transitive dependencies.",
    fields = {"files": "depset of generated source files"},
)

# Source attributes of C++ and Rust rules whose generated files the aspect collects.
_SRC_ATTRS = ["srcs", "hdrs", "textual_hdrs"]

# Dependency attributes the aspect follows. Exec-configured attributes such as proc_macro_deps
# and tools are left out: their files land outside bazel-bin, where editors don't look.
_DEP_ATTRS = ["deps", "implementation_deps", "interface_deps", "link_deps", "crate"]

# Extensions of the generated files an editor navigates to: headers and Rust sources. Generated
# C++ sources are left out; some, such as those embedding Pyodide, are tens of megabytes.
_EXTENSIONS = ["h", "hh", "hpp", "hxx", "h++", "inc", "rs"]

# Rules without CcInfo or CrateInfo whose dependencies are C++ or Rust.
_CC_KINDS = ["cc_binary", "cc_test"]

def _generated(files):
    return [f for f in files if not f.is_source and f.extension in _EXTENSIONS]

def _generated_srcs_aspect_impl(target, ctx):
    if not (CcInfo in target or CrateInfo in target or TestCrateInfo in target or
            ctx.rule.kind in _CC_KINDS):
        files = depset()
        return [GeneratedSrcsInfo(files = files), OutputGroupInfo(generated_srcs = files)]

    direct = []
    for attr in _SRC_ATTRS:
        direct.extend(_generated(getattr(ctx.rule.files, attr, [])))
    if CcInfo in target:
        cc = target[CcInfo].compilation_context

        # Includes the _virtual_includes symlinks of strip_include_prefix and include_prefix.
        direct.extend(_generated(cc.direct_headers))
        direct.extend(_generated(cc.direct_textual_headers))

    transitive = []
    for attr in _DEP_ATTRS:
        deps = getattr(ctx.rule.attr, attr, None)
        if deps == None:
            continue
        if type(deps) != "list":
            deps = [deps]
        for dep in deps:
            if type(dep) == "Target" and GeneratedSrcsInfo in dep:
                transitive.append(dep[GeneratedSrcsInfo].files)

    files = depset(direct, transitive = transitive)
    return [
        GeneratedSrcsInfo(files = files),
        OutputGroupInfo(generated_srcs = files),
    ]

generated_srcs_aspect = aspect(
    implementation = _generated_srcs_aspect_impl,
    attr_aspects = _DEP_ATTRS,
)
