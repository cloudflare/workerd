"""
Bazel rules for generating compilation caches from a list of input files.
"""

load("@bazel_skylib//rules:run_binary.bzl", "run_binary")

def _gen_compile_cache_impl(ctx):
    file_list_path = "{}-compile-cache-file-list".format(ctx.label)
    file_list = ctx.actions.declare_file(file_list_path)

    # Get the File objects from the labels
    input_files = []
    for src in ctx.attr.srcs:
        input_files.extend(src.files.to_list())

    ctx.actions.write(
        output = file_list,
        content = "\n".join([f.short_path for f in input_files]) + "\n",
    )

    output = ctx.actions.declare_file(ctx.attr.output)

    ctx.actions.run(
        outputs = [output],
        inputs = input_files + [file_list],
        arguments = [file_list.path],
        executable = ctx.executable._tool,
        tools = [ctx.executable._tool],
    )

gen_compile_cache = rule(
    implementation = _gen_compile_cache_impl,
    attrs = {
        "srcs": attr.label_list(mandatory = True, allow_files = True),
        "output": attr.string(mandatory = True),
        "_tool": attr.label(executable = True, allow_single_file = True, cfg = "exec", default = "//src/workerd/tools:create_compile_cache"),
    },
)

def wd_compile_cache(name, srcs):
    gen_compile_cache(
        name = name,
        srcs = srcs,
        output = "{}-compile-cache-output".format(name),
    )
