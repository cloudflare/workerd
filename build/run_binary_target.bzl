# Workaround for bazel not supporting a shared exec and target configuration, even when they are
# identical. https://github.com/bazelbuild/bazel/issues/14848
# Derived from the tensorflow project (https://github.com/tensorflow/tensorflow/blob/master/tensorflow/python/tools/api/generator/api_gen.bzl)
# See https://github.com/tensorflow/tensorflow/issues/60167 for discussion

def _run_binary_target_impl(ctx):
    tool = ctx.attr.tool[DefaultInfo].files_to_run.executable
    flags = [ ctx.expand_location(a) if "$(location" in a else a for a in ctx.attr.args ]

    cmd = " ".join([tool.path] + flags)
    ctx.actions.run_shell(
        inputs = ctx.files.srcs,
        outputs = ctx.outputs.outs,
        tools = [tool],
        use_default_shell_env = True,
        command = cmd,
    )

run_binary_target = rule(
    implementation = _run_binary_target_impl,
    attrs = {
        "outs": attr.output_list(mandatory = True),
        "srcs": attr.label_list(allow_files = True),
        "args": attr.string_list(),
        # Setting the configuration to "target" to avoid compiling code used in both this
        # generator-like target and regular targets twice. For cross-compilation this would need to
        # be set to "exec".
        # Unfortunately bazel makes it very difficult to set the configuration at build time as
        # macros are resolved before select() can be resolved based on the command line. This could
        # alternatively be done by defining build targets and the rules used to declare them twice
        # (once for exec and for target).
        "tool": attr.label(
            executable = True,
            cfg = "target",
            mandatory = True),
    },
)
