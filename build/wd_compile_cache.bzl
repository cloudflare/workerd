"""
Bazel rules for generating compilation caches from a list of input files.
"""

def compile_cache_name(file_path):
    return file_path + "_cache"

def _gen_compile_cache_impl(ctx):
    file_list = ctx.actions.declare_file("in")

    # Get the File objects from the labels
    srcs = []
    for src in ctx.attr.srcs:
        srcs.extend(src.files.to_list())

    outs = [compile_cache_name(src.basename) for src in srcs]
    outs = [ctx.actions.declare_file(compile_cache_name(src.basename)) for src in srcs]

    content = []
    for i in range(0, len(srcs)):
        content.append("{} {}".format(srcs[i].path, outs[i].path))

    ctx.actions.write(
        output = file_list,
        content = "\n".join(content) + "\n",
    )

    args = ctx.actions.args()
    args.add(file_list)

    ctx.actions.run_shell(
        outputs = outs,
        inputs = [file_list] + srcs,
        command = ctx.executable._tool.path + " $@",
        arguments = [args],
        use_default_shell_env = True,
        tools = [ctx.executable._tool],
    )

    return [
        DefaultInfo(files = depset(direct = outs)),
    ]

gen_compile_cache = rule(
    implementation = _gen_compile_cache_impl,
    attrs = {
        "srcs": attr.label_list(mandatory = True, allow_files = True),
        "_tool": attr.label(
            executable = True,
            allow_single_file = True,
            cfg = "target",
            default = "//src/workerd/tools:create_compile_cache",
        ),
    },
)

def wd_compile_cache(name, srcs):
    gen_compile_cache(
        name = name,
        srcs = srcs,
        visibility = ["//visibility:public"],
    )
