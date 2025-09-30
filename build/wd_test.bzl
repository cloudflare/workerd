load("@aspect_rules_ts//ts:defs.bzl", "ts_config", "ts_project")

def wd_test(
        src,
        data = [],
        name = None,
        args = [],
        ts_deps = [],
        python_snapshot_test = False,
        **kwargs):
    """Rule to define tests that run `workerd test` with a particular config.

    Args:
     src: A .capnp config file defining the test. (`name` will be derived from this if not
        specified.) The extension `.wd-test` is also permitted instead of `.capnp`, in order to
        avoid confusing other build systems that may assume a `.capnp` file should be compiled.
     data: Additional files which the .capnp config file may embed. All TypeScript files will be compiled,
     their resulting files will be passed to the test as well. Usually TypeScript or Javascript source files.
     args: Additional arguments to pass to `workerd`. Typically used to pass `--experimental`.
    """

    # Add workerd binary to "data" dependencies.
    data = data + [src, "//src/workerd/server:workerd_cross"]

    ts_srcs = [src for src in data if src.endswith(".ts")]

    # Default name based on src.
    if name == None:
        name = src.removesuffix(".capnp").removesuffix(".wd-test").removesuffix(".ts-wd-test")

    if len(ts_srcs) != 0:
        # Generated declarations are currently not being used, but required based on https://github.com/aspect-build/rules_ts/issues/719
        # TODO(build perf): Consider adopting isolated_typecheck to avoid bottlebecks in TS
        # compilation, see https://github.com/aspect-build/rules_ts/blob/f1b7b83/docs/performance.md#isolated-typecheck.
        # This will require extensive refactoring and we may only want to enable it for some
        # targets, but might be useful if we end up transpiling more code later on.
        ts_config(
            name = name + "@ts_config",
            src = "tsconfig.json",
            deps = ["@workerd//tools:base-tsconfig"],
        )
        ts_project(
            name = name + "@ts_project",
            srcs = ts_srcs,
            tsconfig = ":" + name + "@ts_config",
            allow_js = True,
            source_map = True,
            declaration = True,
            deps = ["//src/node:node@tsproject"] + ts_deps,
        )
        data += [js_src.removesuffix(".ts") + ".js" for js_src in ts_srcs]

    # Add initial arguments for `workerd test` command.
    args = [
        "$(location //src/workerd/server:workerd_cross)",
        "test",
        "$(location {})".format(src),
    ] + args

    _wd_test(
        src = src,
        name = name,
        data = data,
        args = args,
        python_snapshot_test = python_snapshot_test,
        **kwargs
    )

WINDOWS_TEMPLATE = """
@echo off
setlocal EnableDelayedExpansion

REM Run supervisor to start sidecar if specified
if not "{sidecar}" == "" (
    REM These environment variables are processed by the supervisor executable
    set PORTS_TO_ASSIGN={port_bindings}
    set RANDOMIZE_IP={randomize_ip}
    set SIDECAR_COMMAND="{sidecar}"
    powershell -Command \"{supervisor} {runtest}\"
) else (
    {runtest}
)

set TEST_EXIT=!ERRORLEVEL!
exit /b !TEST_EXIT!
"""

SH_TEMPLATE = """#!/bin/sh
set -e

# Run supervisor to start sidecar if specified
if [ ! -z "{sidecar}" ]; then
    # These environment variables are processed by the supervisor executable
    PORTS_TO_ASSIGN={port_bindings} RANDOMIZE_IP={randomize_ip} SIDECAR_COMMAND="{sidecar}" {supervisor} {runtest}
else
    {runtest}
fi
"""

WINDOWS_RUNTEST_NORMAL = """powershell -Command \"%*\" `-dTEST_TMPDIR=$ENV:TEST_TMPDIR"""

SH_RUNTEST_NORMAL = """"$@" -dTEST_TMPDIR=$TEST_TMPDIR"""

# We need variants of the RUN_TEST command for Python memory snapshot tests. We have to invoke
# workerd twice, once with --python-save-snapshot to produce the snapshot and once with
# --python-load-snapshot to use it.
#
# We would like to implement this in py_wd_test and not have to complicate wd_test for it, but
# unfortunately bazel provides no way for a test to create a file that is used by another test. So
# we cannot do this with two separate `wd_test` rules. We _could_ use a build step to create the
# snapshot, but then a failure at this stage would be reported as a build failure when really it
# should count as a test failure. So the only option left is to make this modification to wd_test to
# invoke workerd twice for snapshot tests.

WINDOWS_RUNTEST_SNAPSHOT = """
powershell -Command \"%* --python-save-snapshot $ENV:PYTHON_SAVE_SNAPSHOT_ARGS\" `-dTEST_TMPDIR=$ENV:TEST_TMPDIR
set TEST_EXIT=!ERRORLEVEL!
if !TEST_EXIT! EQU 0 (
    powershell -Command \"%* --python-load-snapshot snapshot.bin\" `-dTEST_TMPDIR=$ENV:TEST_TMPDIR
    set TEST_EXIT=!ERRORLEVEL!
)
"""

SH_RUNTEST_SNAPSHOT = """
echo Creating Python Snapshot
"$@" -dTEST_TMPDIR=$TEST_TMPDIR --python-save-snapshot $PYTHON_SAVE_SNAPSHOT_ARGS
echo ""
echo Using Python Snapshot
"$@" -dTEST_TMPDIR=$TEST_TMPDIR --python-load-snapshot snapshot.bin
"""

def _wd_test_impl(ctx):
    is_windows = ctx.target_platform_has_constraint(ctx.attr._platforms_os_windows[platform_common.ConstraintValueInfo])

    if ctx.file.sidecar and ctx.attr.python_snapshot_test:
        # TODO(later): Implement support for generating a combined script with these two options
        # if we have a test that requires it. Not doing it for now due to complexity.
        return print("sidecar and python_snapshot_test currently cannot be used together")

    # Bazel insists that the rule must actually create the executable that it intends to run; it
    # can't just specify some other executable with some args. OK, fine, we'll use a script that
    # just execs its args.
    if is_windows:
        # Batch script executables must end with ".bat"
        executable = ctx.actions.declare_file("%s_wd_test.bat" % ctx.label.name)
        template = WINDOWS_TEMPLATE
        runtest = WINDOWS_RUNTEST_SNAPSHOT if ctx.attr.python_snapshot_test else WINDOWS_RUNTEST_NORMAL
    else:
        executable = ctx.outputs.executable
        template = SH_TEMPLATE
        runtest = SH_RUNTEST_SNAPSHOT if ctx.attr.python_snapshot_test else SH_RUNTEST_NORMAL

    content = template.format(
        sidecar = ctx.file.sidecar.short_path if ctx.file.sidecar else "",
        runtest = runtest,
        supervisor = ctx.file.sidecar_supervisor.short_path if ctx.file.sidecar_supervisor else "",
        port_bindings = ",".join(ctx.attr.sidecar_port_bindings),
        randomize_ip = "true" if ctx.attr.sidecar_randomize_ip else "false",
    )

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

        runfiles = runfiles.merge(ctx.runfiles(files = [ctx.file.sidecar_supervisor]))

        # Also merge the supervisor's own runfiles if it has any
        default_runfiles = ctx.attr.sidecar_supervisor[DefaultInfo].default_runfiles
        if default_runfiles:
            runfiles = runfiles.merge(default_runfiles)

    # IMPORTANT: The workerd binary must be listed in dependency_attributes
    # to ensure its transitive dependencies (all the C++ source files) are
    # included in the coverage instrumentation. Without this, coverage data
    # won't be collected for the actual workerd implementation code.
    instrumented_files_info = coverage_common.instrumented_files_info(
        ctx,
        source_attributes = ["src", "data"],
        dependency_attributes = ["workerd", "sidecar", "sidecar_supervisor"],
        # Include all file types that might contain testable code
        extensions = ["cc", "c++", "cpp", "cxx", "c", "h", "hh", "hpp", "hxx", "inc", "js", "ts", "mjs", "wd-test", "capnp"],
    )
    environment = {}
    if ctx.attr.python_snapshot_test:
        environment["PYTHON_SAVE_SNAPSHOT_ARGS"] = ""
    if ctx.attr.load_snapshot:
        if ctx.attr.python_snapshot_test:
            environment["PYTHON_SAVE_SNAPSHOT_ARGS"] = "--python-load-snapshot load_snapshot.bin"
        f = ctx.attr.load_snapshot.files.to_list()[0]
        runfiles = runfiles.merge(ctx.runfiles(symlinks = {"load_snapshot.bin": f}))

    return [
        DefaultInfo(
            executable = executable,
            runfiles = runfiles,
        ),
        RunEnvironmentInfo(
            environment = environment,
        ),
        instrumented_files_info,
    ]

_wd_test = rule(
    implementation = _wd_test_impl,
    test = True,
    attrs = {
        # Implicit dependencies used by Bazel to generate coverage reports.
        "_lcov_merger": attr.label(
            default = configuration_field(fragment = "coverage", name = "output_generator"),
            executable = True,
            cfg = config.exec(exec_group = "test"),
        ),
        "_collect_cc_coverage": attr.label(
            default = "@bazel_tools//tools/test:collect_cc_coverage",
            executable = True,
            cfg = config.exec(exec_group = "test"),
        ),
        # Source file
        "src": attr.label(allow_single_file = True),
        # The workerd executable is used to run all tests
        "workerd": attr.label(
            allow_single_file = True,
            executable = True,
            cfg = "exec",
            default = "//src/workerd/server:workerd_cross",
        ),
        # A list of files that this test requires to be present in order to run.
        "data": attr.label_list(allow_files = True),
        # If set, an executable that is run in parallel with the test, and provides some functionality
        # needed for the test. This is usually a backend server, with workerd serving as the client.
        # The sidecar will be killed once the test completes.
        "sidecar": attr.label(
            allow_single_file = True,
            executable = True,
            cfg = "exec",
        ),
        # Sidecars
        # ---------
        # For detailed documentation, see src/workerd/api/node/tests/sidecar-supervisor.mjs

        # A list of binding names which will be filled in with random port numbers that the sidecar
        # and test can use for communication. The test will only begin once the sidecar is
        # listening to these ports.
        #
        # In the sidecar, access these bindings as environment variables. In the wd-test file, add
        # fromEnvironment bindings to expose them to the test.
        #
        # Reminder: you'll also need to add a network = ( allow = ["private"] ) service as well.
        "sidecar_port_bindings": attr.string_list(),
        # If true, a random IP address will be assigned to the sidecar process, and provided in the
        # environment variable SIDECAR_HOSTNAME,
        "sidecar_randomize_ip": attr.bool(default = True),
        # An executable that is used to manage port assignments and child process creation when a
        # sidecar is specified.
        "sidecar_supervisor": attr.label(
            allow_single_file = True,
            executable = True,
            cfg = "exec",
            default = "//src/workerd/api/node/tests:sidecar-supervisor",
        ),
        "python_snapshot_test": attr.bool(),
        "load_snapshot": attr.label(allow_single_file = True),
        # A reference to the Windows platform label, needed for the implementation of wd_test
        "_platforms_os_windows": attr.label(default = "@platforms//os:windows"),
    },
)
