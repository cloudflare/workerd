"""wd_bench definition"""

load("@workerd//:build/lint_test.bzl", "lint_test")
load("@workerd//:build/wd_test.bzl", "wd_cli_test")

_WORKERD = "//src/workerd/server:workerd_cross"

def wd_bench(
        src,
        data = [],
        name = None,
        args = [],
        lint = True,
        compat_date = "",
        generate_all_compat_flags_variant = True,
        generate_all_autogates_variant = True,
        tags = [],
        **kwargs):
    """Defines benchmarks that run `workerd bench` with a particular config.

    Args:
     src: A config file defining the benchmarks, in the same format as wd_test()'s. (`name` is
        derived from it if not specified.) The extensions `.wd-bench`, `.wd-test`, and `.capnp` are
        permitted.
     data: Additional files that the config file may embed. TypeScript files are compiled, as for
        wd_test().
     args: Additional arguments to pass to `workerd bench`.
     lint: If True (default), lint the JavaScript and TypeScript in `data`.
     compat_date: If specified, the compat date for the default variant instead of `latest`, the
        newest that the build supports.
     generate_all_compat_flags_variant: If True (default), generate @all-compat-flags variants.
     generate_all_autogates_variant: If True (default), generate @all-autogates variants.
     tags: Tags for all targets.

    The variants are `name@` (`compat_date`), `name@all-compat-flags` (2999-12-31), and
    `name@all-autogates` (`compat_date` with all autogates). For each variant, this generates:
     - <variant>: a target for `bazel run`, which measures the benchmarks and prints the results.
       Arguments after `--` are passed to `workerd bench`, e.g. a filter or `--format=json`. It is
       tagged `workerd-benchmark`.
     - <variant>@smoke: a test that runs the benchmarks once each with `--quick`, to check that they
       work. This isn't tagged `workerd-benchmark`, so CI runs it.
    It also generates `name@benchmark.json`, the default variant's report as a build output, which
    is off by default.

    As for wd_test(), a writable disk service named TEST_TMPDIR, e.g. for Durable Object storage,
    gets a new empty directory for every run. Any other disk service uses the path in the config.
    """
    data = data + [src]

    ts_srcs = [s for s in data if s.endswith(".ts")]
    if ts_srcs:
        data = data + [s.removesuffix(".ts") + ".js" for s in ts_srcs]

    if name == None:
        name = src.removesuffix(".capnp").removesuffix(".wd-test").removesuffix(".wd-bench")

    if lint:
        lint_srcs = [s for s in data if (s.endswith(".ts") or s.endswith(".mts") or s.endswith(".js") or s.endswith(".mjs")) and not s.startswith("../") and not s.startswith("//")]
        if lint_srcs:
            lint_test(
                name = name,
                eslintrc_json = "@workerd//tools:base.eslint.config.mjs",
                tsconfig_json = "@workerd//tools:base.tsconfig.json",
                srcs = lint_srcs,
                data = ["tsconfig.json"] if ts_srcs else [],
                no_copy_to_bin = [
                    "@workerd//tools:base.eslint.config.mjs",
                    "@workerd//tools:base.tsconfig.json",
                ],
            )

    # Benchmarks usually measure current behavior, so unlike wd_test(), the default variant uses
    # the newest compat date that the build supports rather than the oldest.
    default_compat_args = ["--compat-date={}".format(compat_date or "latest")]
    variants = [("@", default_compat_args)]
    if generate_all_compat_flags_variant:
        variants.append(("@all-compat-flags", ["--compat-date=2999-12-31"]))
    if generate_all_autogates_variant:
        variants.append(("@all-autogates", default_compat_args + ["--all-autogates"]))

    workerd_args = ["$(location {})".format(_WORKERD), "bench", "$(location {})".format(src)] + args
    for suffix, variant_args in variants:
        _wd_bench_run(
            name = name + suffix,
            data = data + [_WORKERD],
            args = workerd_args + variant_args,
            tags = ["workerd-benchmark"] + tags,
            **kwargs
        )
        wd_cli_test(
            name = name + suffix.removesuffix("@") + "@smoke",
            src = src,
            data = data + [_WORKERD],
            args = workerd_args + variant_args + ["--quick"],
            tags = tags,
            **kwargs
        )

    native.genrule(
        name = name + "@benchmark.json",
        # workerd is a source rather than a tool so that this uses the build that the other targets
        # use, as wd_cc_benchmark()'s report does, rather than building it again for the exec
        # configuration.
        srcs = data + [_WORKERD],
        outs = [name + ".benchmark.json"],
        cmd = ("TEST_TMPDIR=$$(mktemp -d) && " +
               "$(location {}) bench --format=json --output=\"$@\" $(location {}) {} " +
               "-dTEST_TMPDIR=$$TEST_TMPDIR; status=$$?; rm -rf $$TEST_TMPDIR; exit $$status").format(
            _WORKERD,
            src,
            " ".join(args + default_compat_args),
        ),
        tags = ["off-by-default", "benchmark_report", "workerd-benchmark"] + tags,
    )

# As for wd_test(), configs can declare a writable disk service named TEST_TMPDIR, e.g. for
# Durable Object storage. The smoke tests get Bazel's test temporary directory from wd_cli_test();
# the run targets and the report create a new temporary directory and remove it afterwards.
_SH_TEMPLATE = """#!/bin/bash
set -euo pipefail
TEST_TMPDIR=$(mktemp -d)
trap 'rm -rf "$TEST_TMPDIR"' EXIT
"$@" -dTEST_TMPDIR="$TEST_TMPDIR"
"""

_WINDOWS_TEMPLATE = """@echo off
setlocal
set "WD_BENCH_TMPDIR=%TEMP%\\wd-bench-%RANDOM%%RANDOM%"
mkdir "%WD_BENCH_TMPDIR%" || exit /b 1
%* -dTEST_TMPDIR=%WD_BENCH_TMPDIR%
set "STATUS=%ERRORLEVEL%"
rmdir /s /q "%WD_BENCH_TMPDIR%"
exit /b %STATUS%
"""

def _wd_bench_run_impl(ctx):
    is_windows = ctx.target_platform_has_constraint(ctx.attr._platforms_os_windows[platform_common.ConstraintValueInfo])
    if is_windows:
        executable = ctx.actions.declare_file("%s_wd_bench.bat" % ctx.label.name)
        content = _WINDOWS_TEMPLATE
    else:
        executable = ctx.actions.declare_file("%s_wd_bench.sh" % ctx.label.name)
        content = _SH_TEMPLATE
    ctx.actions.write(output = executable, content = content, is_executable = True)

    runfiles = ctx.runfiles(files = ctx.files.data)
    for target in ctx.attr.data:
        runfiles = runfiles.merge(target[DefaultInfo].default_runfiles)
    return [DefaultInfo(executable = executable, runfiles = runfiles)]

# Bazel runs only executables that the rule creates, so this writes a script that runs its
# arguments, which wd_bench() sets to the workerd command line.
_wd_bench_run = rule(
    implementation = _wd_bench_run_impl,
    executable = True,
    attrs = {
        "data": attr.label_list(allow_files = True),
        "_platforms_os_windows": attr.label(default = "@platforms//os:windows"),
    },
)
