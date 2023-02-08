def wd_test(
        src,
        data = [],
        name = None,
        args = [],
        **kwargs):
    """Rule to define tests that run `workerd test` with a particular config.

    Args:
     src: A .capnp config file defining the test. (`name` will be derived from this if not
        specified.) The extension `.wd-test` is also permitted instead of `.capnp`, in order to
        avoid confusing other build systems that may assume a `.capnp` file should be complied.
     data: Files which the .capnp config file may embed. Typically JavaScript files.
     args: Additional arguments to pass to `workerd`. Typically used to pass `--experimental`.
    """

    # Add workerd binary to "data" dependencies.
    data = data + [src, "//src/workerd/server:workerd"]

    # Add initial arguments for `workerd test` command.
    args = [
        "$(location //src/workerd/server:workerd)",
        "test",
        "$(location {})".format(src),
    ] + args

    # Default name based on src.
    if name == None:
        name = src.removesuffix(".capnp").removesuffix(".wd-test")

    _wd_test(
        name = name,
        data = data,
        args = args,
        **kwargs
    )

def _wd_test_impl(ctx):
    # Bazel insists that the rule must actually create the executable that it intends to run; it
    # can't just specify some other executable with some args. OK, fine, we'll use a script that
    # just execs its args.
    ctx.actions.write(
        output = ctx.outputs.executable,
        content = "#! /bin/sh\nexec \"$@\"\n",
        is_executable = True,
    )

    return [
        DefaultInfo(
            executable = ctx.outputs.executable,
            runfiles = ctx.runfiles(files = ctx.files.data)
        ),
    ]

_wd_test = rule(
    implementation = _wd_test_impl,
    test = True,
    attrs = {
        "workerd": attr.label(
            allow_single_file = True,
            executable = True,
            cfg = "exec",
            default = "//src/workerd/server:workerd",
        ),
        "flags": attr.string_list(),
        "data": attr.label_list(allow_files = True),
    },
)
