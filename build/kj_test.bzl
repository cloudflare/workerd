load("@rules_cc//cc:cc_binary.bzl", "cc_binary")

def kj_test(
        src,
        data = [],
        deps = [],
        tags = [],
        size = "medium",
        **kwargs):
    test_name = src.removesuffix(".c++")
    binary_name = test_name + "_binary"
    cc_binary(
        name = binary_name,
        testonly = True,
        srcs = [src],
        deps = [
            "@capnp-cpp//src/kj:kj-test",
        ] + deps,
        linkstatic = select({
            "@platforms//os:linux": 0,
            "//conditions:default": 1,
        }),
        # For test binaries, reduce thinLTO optimizations and inlining to speed up linking. This
        # only has an effect if thinLTO is enabled. Also apply dead_strip on macOS to manage binary
        # sizes.
        linkopts = select({
            "@platforms//os:linux": ["-Wl,--lto-O1", "-Wl,-mllvm,-import-instr-limit=5"],
            "@//:use_dead_strip": ["-Wl,-dead_strip", "-Wl,-no_exported_symbols"],
            "//conditions:default": [""],
        }),
        data = data,
        tags = tags,
        target_compatible_with = select({
            "@//build/config:no_build": ["@platforms//:incompatible"],
            "//conditions:default": [],
        }),
        **kwargs
    )

    pkg = native.package_name()
    cross_alias = binary_name + "_cross"
    native.alias(
        name = cross_alias,
        actual = select({
            "@//build/config:prebuilt_binaries_arm64": "@//:bin.arm64/tmp/workerd/{}/{}.aarch64-linux-gnu".format(pkg, binary_name),
            "//conditions:default": binary_name,
        }),
    )

    _kj_test_wrapper_test(
        name = test_name + "@",
        size = size,
        binary = cross_alias,
        data = data,
        tags = tags,
    )
    _kj_test_wrapper_test(
        name = test_name + "@all-autogates",
        size = size,
        binary = cross_alias,
        data = data,
        tags = tags,
        env = {"WORKERD_ALL_AUTOGATES": "1"},
    )

def _kj_test_impl(ctx):
    is_windows = ctx.target_platform_has_constraint(ctx.attr._platforms_os_windows[platform_common.ConstraintValueInfo])
    binary_file = ctx.attr.binary.files.to_list()[0]

    # The test wrapper script sets up coverage environment and runs the binary
    if is_windows:
        executable = ctx.actions.declare_file("%s_kj_test.bat" % ctx.label.name)
        content = """@echo off
if defined COVERAGE_DIR (
    set LLVM_PROFILE_FILE=%COVERAGE_DIR%\\coverage.profraw
)
{}
""".format(binary_file.short_path)
    else:
        executable = ctx.outputs.executable
        content = """#!/bin/sh
if [ -n "$COVERAGE_DIR" ]; then
    export LLVM_PROFILE_FILE="$COVERAGE_DIR/coverage.profraw"
fi
exec "{}"
""".format(binary_file.short_path)

    ctx.actions.write(
        output = executable,
        content = content,
        is_executable = True,
    )

    runfiles = ctx.runfiles(files = [binary_file] + ctx.files.data)

    # Merge runfiles from the binary
    default_runfiles = ctx.attr.binary[DefaultInfo].default_runfiles
    if default_runfiles:
        runfiles = runfiles.merge(default_runfiles)

    # Set up coverage instrumentation
    instrumented_files_info = coverage_common.instrumented_files_info(
        ctx,
        dependency_attributes = ["binary"],
        extensions = ["c++", "cc", "cpp", "cxx", "c", "h", "hh", "hpp", "hxx", "inc"],
    )

    return [
        DefaultInfo(
            executable = executable,
            runfiles = runfiles,
        ),
        RunEnvironmentInfo(
            environment = ctx.attr.env,
        ),
        instrumented_files_info,
    ]

_kj_test_wrapper_test = rule(
    implementation = _kj_test_impl,
    test = True,
    executable = True,
    attrs = {
        "binary": attr.label(
            allow_single_file = True,
            executable = True,
            cfg = "target",
            mandatory = True,
        ),
        "data": attr.label_list(allow_files = True),
        "env": attr.string_dict(default = {}),
        "_platforms_os_windows": attr.label(default = "@platforms//os:windows"),
        # Coverage collection attributes
        "_lcov_merger": attr.label(
            default = configuration_field(fragment = "coverage", name = "output_generator"),
            executable = True,
            cfg = "exec",
        ),
        "_collect_cc_coverage": attr.label(
            default = "@bazel_tools//tools/test:collect_cc_coverage",
            executable = True,
            cfg = "exec",
        ),
    },
)
