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

    _kj_test(
        name = test_name + "@",
        size = size,
        binary = cross_alias,
        data = data,
        tags = tags,
    )

    _kj_test(
        name = test_name + "@all-autogates",
        size = size,
        env = {"WORKERD_ALL_AUTOGATES": "1"},
        binary = cross_alias,
        data = data,
        tags = tags,
    )

# Shell template for kj_test - sets up coverage environment for the subprocess
_SH_TEMPLATE = """#!/bin/sh
set -e
{env_exports}
# Set up coverage for the test binary subprocess
if [ -n "$COVERAGE_DIR" ]; then
    # Fix directory permissions for coverage post-processing
    # (Bazel may create COVERAGE_DIR with read-only permissions)
    chmod -R u+w "$COVERAGE_DIR" 2>/dev/null || true
    export LLVM_PROFILE_FILE="$COVERAGE_DIR/%p-%m.profraw"
    export KJ_CLEAN_SHUTDOWN=1
fi

exec {binary} "$@"
"""

_BAT_TEMPLATE = """@echo off
{env_exports}{binary} %*
"""

def _kj_test_impl(ctx):
    is_windows = ctx.target_platform_has_constraint(ctx.attr._platforms_os_windows[platform_common.ConstraintValueInfo])

    # Generate environment variable exports
    env_exports = ""
    for key, value in ctx.attr.env.items():
        if is_windows:
            env_exports += "set {}={}\n".format(key, value)
        else:
            env_exports += "export {}=\"{}\"\n".format(key, value)

    if is_windows:
        executable = ctx.actions.declare_file("%s_kj_test.bat" % ctx.label.name)
        content = _BAT_TEMPLATE.format(binary = ctx.file.binary.short_path.replace("/", "\\"), env_exports = env_exports)
    else:
        executable = ctx.outputs.executable
        content = _SH_TEMPLATE.format(binary = ctx.file.binary.short_path, env_exports = env_exports)

    ctx.actions.write(
        output = executable,
        content = content,
        is_executable = True,
    )

    runfiles = ctx.runfiles(files = ctx.files.data + [ctx.file.binary])

    # Merge the binary's runfiles
    default_runfiles = ctx.attr.binary[DefaultInfo].default_runfiles
    if default_runfiles:
        runfiles = runfiles.merge(default_runfiles)

    # IMPORTANT: The binary must be listed in dependency_attributes to ensure
    # its transitive dependencies (all the C++ source files) are included in
    # coverage instrumentation. Without this, coverage data won't be collected.
    instrumented_files_info = coverage_common.instrumented_files_info(
        ctx,
        source_attributes = [],
        dependency_attributes = ["binary"],
    )

    return [
        DefaultInfo(
            executable = executable,
            runfiles = runfiles,
        ),
        instrumented_files_info,
    ]

_kj_test = rule(
    implementation = _kj_test_impl,
    test = True,
    attrs = {
        # Implicit dependencies used by Bazel to generate coverage reports.
        "_lcov_merger": attr.label(
            default = configuration_field(fragment = "coverage", name = "output_generator"),
            executable = True,
            cfg = config.exec(exec_group = "test"),
        ),
        "_collect_cc_coverage": attr.label(
            default = "//build/cc_coverage:collect_cc_coverage",
            executable = True,
            cfg = config.exec(exec_group = "test"),
        ),
        # The test binary to run
        "binary": attr.label(
            allow_single_file = True,
            executable = True,
            cfg = "target",
            mandatory = True,
        ),
        # Additional data files needed by the test
        "data": attr.label_list(allow_files = True),
        # Environment variables to set when running the test
        "env": attr.string_dict(default = {}),
        # Reference to Windows platform for cross-platform support
        "_platforms_os_windows": attr.label(default = "@platforms//os:windows"),
    },
)
