load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("//:build/linking.bzl", "CC_TEST_LINKSTATIC")
load("//build/tools/llvm:coverage.bzl", "COVERAGE_TOOLCHAINS", "coverage_llvm_tools")

def kj_test(
        src,
        data = [],
        deps = [],
        tags = [],
        size = "medium",
        target_compatible_with = None,
        **kwargs):
    test_name = src.removesuffix(".c++")
    binary_name = test_name + "_binary"
    cc_binary(
        name = binary_name,
        testonly = True,
        srcs = [src],
        deps = [
            "@capnp-cpp//src/kj:kj-test",
            "//build/deps:linkopts_default",
            # The tokio kj::setupAsyncIo() is linked per BINARY: every test binary links it here, and
            # no library carries it, so a binary can never end up with two event-loop
            # implementations in its link.
            "//src/workerd/util:setup-async-io",
        ] + deps,
        linkstatic = CC_TEST_LINKSTATIC,
        data = data,
        # Tag with cpu:2 since this target depends on linkopts_default.
        tags = tags + ["cpu:2"],
        target_compatible_with = target_compatible_with,
        **kwargs
    )

    _kj_test(
        name = test_name + "@",
        binary = binary_name,
        data = data,
        tags = tags,
        size = size,
        target_compatible_with = target_compatible_with,
    )

    _kj_test(
        name = test_name + "@all-autogates",
        binary = binary_name,
        data = data,
        env = {"WORKERD_ALL_AUTOGATES": "1"},
        # Tag with no-coverage to reduce coverage CI time
        tags = tags + ["no-coverage"],
        size = size,
        target_compatible_with = target_compatible_with,
    )

def _kj_test_impl(ctx):
    # A test rule runs an executable of its own, so this one is the binary under the test's name;
    # the test variants share the binary instead of each linking it.
    binary = ctx.executable.binary
    executable = ctx.actions.declare_file(ctx.label.name + ("." + binary.extension if binary.extension else ""))
    ctx.actions.symlink(output = executable, target_file = binary, is_executable = True)
    environment = dict(ctx.attr.env)
    runfiles = ctx.runfiles(files = ctx.files.data).merge_all(
        [t[DefaultInfo].default_runfiles for t in [ctx.attr.binary] + ctx.attr.data],
    )
    runfiles = runfiles.merge(coverage_llvm_tools(ctx, executable, environment))
    return [
        DefaultInfo(executable = executable, runfiles = runfiles),
        RunEnvironmentInfo(environment = environment),
        coverage_common.instrumented_files_info(ctx, dependency_attributes = ["binary", "data"]),
    ]

_kj_test = rule(
    implementation = _kj_test_impl,
    test = True,
    toolchains = COVERAGE_TOOLCHAINS,
    attrs = {
        "binary": attr.label(executable = True, cfg = "target"),
        "data": attr.label_list(allow_files = True),
        "env": attr.string_dict(),
        # Implicit dependencies used by Bazel to generate coverage reports, as in wd_test.
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
    },
)
