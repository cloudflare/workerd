load("@aspect_rules_ts//ts:defs.bzl", "ts_project")

def wd_test(
        src,
        data = [],
        name = None,
        args = [],
        ts_deps = [],
        **kwargs):
    """Rule to define tests that run `workerd test` with a particular config.

    Args:
     src: A .capnp config file defining the test. (`name` will be derived from this if not
        specified.) The extension `.wd-test` is also permitted instead of `.capnp`, in order to
        avoid confusing other build systems that may assume a `.capnp` file should be compiled. As
        an extension, `.gpu-wd-test` is supported to enable special handling for GPU tests.
     data: Additional files which the .capnp config file may embed. All TypeScript files will be compiled,
     their resulting files will be passed to the test as well. Usually TypeScript or Javascript source files.
     args: Additional arguments to pass to `workerd`. Typically used to pass `--experimental`.
    """

    # Add workerd binary to "data" dependencies.
    data = data + [src, "//src/workerd/server:workerd"]

    ts_srcs = [src for src in data if src.endswith(".ts")]

    # Default name based on src.
    if name == None:
        name = src.removesuffix(".capnp").removesuffix(".wd-test").removesuffix(".gpu-wd-test").removesuffix(".ts-wd-test")

    if len(ts_srcs) != 0:
        # Generated declarations are currently not being used, but required based on https://github.com/aspect-build/rules_ts/issues/719
        # TODO When TypeScript 5.6 comes out use noCheck so the test fails throwing a type error.
        ts_project(
            name = name + "@ts_project",
            srcs = ts_srcs,
            tsconfig = "tsconfig.json",
            allow_js = True,
            source_map = True,
            composite = True,
            declaration = True,
            deps = ["//src/node:node@tsproject"] + ts_deps,
        )
        data += [js_src.removesuffix(".ts") + ".js" for js_src in ts_srcs]

    # Add initial arguments for `workerd test` command.
    args = [
        "$(location //src/workerd/server:workerd)",
        "test",
        "$(location {})".format(src),
    ] + args

    _wd_test(
        name = name,
        data = data,
        args = args,
        **kwargs
    )

WINDOWS_TEMPLATE = """
@echo off
setlocal EnableDelayedExpansion

REM Start sidecar if specified
if not "%SIDECAR%" == "" (
    start /b "" "%SIDECAR%" > nul 2>&1
    set SIDECAR_PID=!ERRORLEVEL!
    timeout /t 1 > nul
)

REM Run the actual test
powershell -Command \"%*\" `-dTEST_TMPDIR=$ENV:TEST_TMPDIR
set TEST_EXIT=!ERRORLEVEL!

REM Cleanup sidecar if it was started
if defined SIDECAR_PID (
    taskkill /F /PID !SIDECAR_PID! > nul 2>&1
)

exit /b !TEST_EXIT!
"""

SH_TEMPLATE = """#!/bin/sh
set -e

cleanup() {
    if [ ! -z "$SIDECAR_PID" ]; then
        kill $SIDECAR_PID 2>/dev/null || true
    fi
}

trap cleanup EXIT

# Start sidecar if specified
if [ ! -z "$(SIDECAR)" ]; then
    "$(SIDECAR)" & SIDECAR_PID=$!
    # Wait until the process is ready
    sleep 3
fi

# Run the actual test
"$@" -dTEST_TMPDIR=$TEST_TMPDIR
"""

def _wd_test_impl(ctx):
    is_windows = ctx.target_platform_has_constraint(ctx.attr._platforms_os_windows[platform_common.ConstraintValueInfo])

    # Bazel insists that the rule must actually create the executable that it intends to run; it
    # can't just specify some other executable with some args. OK, fine, we'll use a script that
    # just execs its args.
    if is_windows:
        # Batch script executables must end with ".bat"
        executable = ctx.actions.declare_file("%s_wd_test.bat" % ctx.label.name)
        content = WINDOWS_TEMPLATE.replace("$(SIDECAR)", ctx.file.sidecar.path if ctx.file.sidecar else "")
    else:
        executable = ctx.outputs.executable
        content = SH_TEMPLATE.replace("$(SIDECAR)", ctx.file.sidecar.short_path if ctx.file.sidecar else "")

    ctx.actions.write(
        output = executable,
        content = content,
        is_executable = True,
    )

    runfiles = ctx.runfiles(files = ctx.files.data)
    if ctx.file.sidecar:
        runfiles = runfiles.merge(ctx.runfiles(files = [ctx.file.sidecar]))

        # Also merge the sidecar's own runfiles if it has any
        default_runfiles = ctx.attr.sidecar[DefaultInfo].default_runfiles
        if default_runfiles:
            runfiles = runfiles.merge(default_runfiles)

    return [
        DefaultInfo(
            executable = executable,
            runfiles = runfiles,
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
        "sidecar": attr.label(
            allow_single_file = True,
            executable = True,
            cfg = "exec",
        ),
        "_platforms_os_windows": attr.label(default = "@platforms//os:windows"),
    },
)
