"""
Bazel rule to compile .capnp files into JS/TS using capnp-es.
Based on https://github.com/capnproto/capnproto/blob/3b2e368cecc4b1419b40c5970d74a7a342224fac/c++/src/capnp/cc_capnp_library.bzl.
"""

load("@aspect_rules_js//js:defs.bzl", "js_library")

capnp_provider = provider("Capnproto Provider", fields = {
    "includes": "includes for this target (transitive)",
    "inputs": "src + data for the target",
    "src_prefix": "src_prefix of the target",
})

def _workspace_path(label, path):
    if label.workspace_root == "":
        return path
    return label.workspace_root + "/" + path

def _capnp_plugin_gen(ctx, output_format, out_dir, inputs, includes, src_prefix, system_include):
    """Generate output files of the given output_format ("js" or "ts")"""

    if not (output_format == "js" or output_format == "ts"):
        fail("Only js and ts output formats are supported")

    # Filter the outputs to generate by requested output_format
    outputs = [out for out in ctx.outputs.outs if out.path.endswith(".%s" % output_format)]
    if (not outputs):
        return

    plugin_name = "_%s_plugin" % output_format
    plugin_executable = getattr(ctx.executable, plugin_name)
    plugin_files = getattr(ctx.files, plugin_name)

    label = ctx.label

    js_out = "-o%s:%s" % (plugin_executable.path, out_dir)
    args = ctx.actions.args()
    args.add_all(["compile", "--verbose", js_out])
    args.add_all(["-I" + inc for inc in includes])
    args.add_all(["-I", system_include])
    if src_prefix != "":
        args.add_all(["--src-prefix", src_prefix])

    args.add_all([s for s in ctx.files.srcs])

    ctx.actions.run(
        inputs = inputs + plugin_files + ctx.files._capnpc_capnp + ctx.files._capnp_system,
        tools = [plugin_executable],  # Include required js_binary runfiles
        outputs = outputs,
        executable = ctx.executable._capnpc,
        arguments = [args],
        mnemonic = "GenCapnp",
    )

def _capnp_gen_impl(ctx):
    label = ctx.label
    src_prefix = _workspace_path(label, ctx.attr.src_prefix)
    includes = []

    inputs = ctx.files.srcs + ctx.files.data
    for dep_target in ctx.attr.deps:
        includes += dep_target[capnp_provider].includes
        inputs += dep_target[capnp_provider].inputs

    if src_prefix != "":
        includes.append(src_prefix)

    system_include = ctx.files._capnp_system[0].dirname.removesuffix("/capnp")

    out_dir = ctx.var["GENDIR"]
    if src_prefix != "":
        out_dir = out_dir + "/" + src_prefix

    _capnp_plugin_gen(
        ctx,
        output_format = "js",
        out_dir = out_dir,
        inputs = inputs,
        includes = includes,
        src_prefix = src_prefix,
        system_include = system_include,
    )

    _capnp_plugin_gen(
        ctx,
        output_format = "ts",
        out_dir = out_dir,
        inputs = inputs,
        includes = includes,
        src_prefix = src_prefix,
        system_include = system_include,
    )

    return [
        capnp_provider(
            includes = includes,
            inputs = inputs,
            src_prefix = src_prefix,
        ),
    ]

_capnp_gen = rule(
    attrs = {
        "srcs": attr.label_list(allow_files = True),
        "deps": attr.label_list(providers = [capnp_provider]),
        "data": attr.label_list(allow_files = True),
        "outs": attr.output_list(),
        "src_prefix": attr.string(),
        "_capnpc": attr.label(executable = True, allow_single_file = True, cfg = "exec", default = "@capnp-cpp//src/capnp:capnp_tool"),
        "_js_plugin": attr.label(executable = True, allow_single_file = True, cfg = "exec", default = "//:capnpc_js_plugin"),
        "_ts_plugin": attr.label(executable = True, allow_single_file = True, cfg = "exec", default = "//:capnpc_ts_plugin"),
        "_capnpc_capnp": attr.label(executable = True, allow_single_file = True, cfg = "exec", default = "@capnp-cpp//src/capnp:capnpc-capnp"),
        "_capnp_system": attr.label(default = "@capnp-cpp//src/capnp:capnp_system_library"),
    },
    output_to_genfiles = True,
    implementation = _capnp_gen_impl,
)

def js_capnp_library(
        name,
        srcs = [],
        data = [],
        outs = [],
        deps = [],
        src_prefix = "",
        visibility = None,
        target_compatible_with = None,
        **kwargs):
    """Bazel rule to create a JavaScript capnproto library from capnp source files

    Args:
        name: library name
        srcs: list of files to compile
        data: additional files to provide to the compiler - data files and includes that need not to
            be compiled
        outs: expected output files - .js and .ts files
        deps: other js_capnp_library rules to depend on
        src_prefix: src_prefix for capnp compiler to the source root
        visibility: rule visibility
        target_compatible_with: target compatibility
        **kwargs: rest of the arguments to js_library rule
    """

    _capnp_gen(
        name = name + "_gen",
        srcs = srcs,
        deps = [s + "_gen" for s in deps],
        data = data,
        outs = outs,
        src_prefix = src_prefix,
        visibility = visibility,
        target_compatible_with = target_compatible_with,
    )
    js_library(
        name = name,
        srcs = outs,
        deps = deps + ["//:node_modules/capnp-es"],
        visibility = visibility,
        target_compatible_with = target_compatible_with,
        **kwargs
    )
