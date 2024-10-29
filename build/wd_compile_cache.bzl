"""
Bazel rules for generating compilation caches from a list of input files.
"""

load("@bazel_skylib//rules:run_binary.bzl", "run_binary")

def _gen_compile_cache_impl(ctx):
    file_list = ctx.actions.declare_file("in")

    # Get the File objects from the labels
    srcs = []
    for src in ctx.attr.srcs:
        srcs.extend(src.files.to_list())

    ctx.actions.write(
        output = file_list,
        content = "\n".join([f.path for f in srcs]) + "\n",
    )

    out = ctx.outputs.out

    args = ctx.actions.args()
    args.add(out)
    args.add(file_list)

    ctx.actions.run(
        outputs = [out],
        inputs = [file_list] + srcs,
        arguments = [args],
        executable = ctx.executable._tool,
        tools = [ctx.executable._tool],
    )

    return [
        DefaultInfo(files = depset([out])),
    ]

gen_compile_cache = rule(
    implementation = _gen_compile_cache_impl,
    attrs = {
        "srcs": attr.label_list(mandatory = True, allow_files = True),
        "out": attr.output(mandatory = True),
        "_tool": attr.label(executable = True, allow_single_file = True, cfg = "exec", default = "//src/workerd/tools:create_compile_cache"),
    },
)

def wd_compile_cache(name, srcs):
    gen_compile_cache(
        name = name + "_gen",
        srcs = srcs,
        out = name,
    )
