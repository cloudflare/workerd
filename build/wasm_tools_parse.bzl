load("@bazel_skylib//lib:paths.bzl", "paths")

def _impl(ctx):
    in_file = ctx.file.src
    out_file = ctx.actions.declare_file(paths.replace_extension(in_file.basename, ".wasm"))
    ctx.actions.run_shell(
        tools = [ctx.executable._wasm_tools],
        inputs = [in_file],
        outputs = [out_file],
        arguments = [
            ctx.executable._wasm_tools.path,
            in_file.path,
            out_file.path,
        ],
        progress_message = "Running wasm-tools parse on %s" % in_file.short_path,
        command = "$1 parse $2 -o $3",
    )

    return [DefaultInfo(files = depset([out_file]))]

wasm_tools_parse = rule(
    implementation = _impl,
    attrs = {
        "src": attr.label(mandatory = True, allow_single_file = True),
        "_wasm_tools": attr.label(
            executable = True,
            allow_files = True,
            cfg = "exec",
            default = Label("//tools:wasm-tools"),
        ),
    },
)
