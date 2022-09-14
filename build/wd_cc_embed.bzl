load("//:build/wd_cc_library.bzl", "wd_cc_library")

def wd_cc_embed_name(file_name):
    return "EMBED_" + file_name.replace(".", "_").replace("-", "_").replace("/", "_")

def _ew_cc_embed_gen_impl(ctx):
    # <OUT_HEADER> <OUT_ASSEMBLY>
    args = [ctx.expand_location("$(execpath {})".format(l.name)) for l in ctx.attr.outs]
    inputs = []
    for ds in ctx.attr.data:
        for f in ds.files.to_list():
            inputs.append(f)

            # <INPUT_FILE> <INPUT_NAME>
            args.append(f.path)
            args.append(f.short_path)

    ctx.actions.run(
        executable = ctx.executable._tool,
        tools = ctx.attr._tool.files,
        inputs = inputs,
        outputs = ctx.outputs.outs,
        arguments = args,
        mnemonic = "GenEmbed",
    )

    return [DefaultInfo(files = depset(ctx.outputs.outs))]

wd_cc_embed_gen = rule(
    implementation = _ew_cc_embed_gen_impl,
    attrs = {
        "data": attr.label_list(allow_files = True, mandatory = True),
        "outs": attr.output_list(),
        "_tool": attr.label(
            allow_single_file = True,
            executable = True,
            cfg = "exec",
            default = "//:build/scripts/gen-embed.sh",
        ),
    },
)

def wd_cc_embed(
        name,
        base_name,
        data,
        **kwargs):
    wd_cc_embed_gen(
        name = name + "_gen",
        data = data,
        outs = [base_name + ".h", base_name + ".S"],
    )

    wd_cc_library(
        name = name,
        srcs = [base_name + ".S"],
        hdrs = [base_name + ".h"],
        textual_hdrs = data,
        **kwargs
    )
