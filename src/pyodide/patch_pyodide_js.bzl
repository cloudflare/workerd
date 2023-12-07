"""
The patch_pyodide_js rule. The rule invokes `patch_pyodide_js.py` with arguments
the input and output paths. Before using it, it's necessary to first make a
`py_binary` from `patch_pyodide_js.py`.
"""


def _impl(ctx):
    args = ctx.actions.args()
    args.add_all(ctx.files.input)
    args.add(ctx.outputs.output)

    ctx.actions.run(
        inputs=ctx.files.input,
        outputs=[ctx.outputs.output],
        arguments=[args],
        progress_message="Generating pyodide.asm.js",
        executable=ctx.executable._gen_tool,
    )


patch_pyodide_js = rule(
    implementation=_impl,
    attrs={
        "input": attr.label(
            allow_single_file=True,
            mandatory=True,
        ),
        "output": attr.output(
            mandatory=True,
        ),
        "_gen_tool": attr.label(
            default=Label(":patch_pyodide_js"),
            executable=True,
            allow_files=True,
            cfg="exec",
        ),
        "deps": attr.label_list(),
    },
)
