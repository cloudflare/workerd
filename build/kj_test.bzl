load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("//:build/linking.bzl", "CC_TEST_LINKSTATIC")
load("//:build/test_attrs.bzl", "test_attrs")
load("//:build/wd_test.bzl", "all_autogates_except_env")
load("//build/tools/llvm:coverage.bzl", "COVERAGE_TOOLCHAINS", "coverage_llvm_tools")

def kj_test(
        src,
        data = [],
        deps = [],
        tags = [],
        size = "medium",
        shard_count = None,
        target_compatible_with = None,
        generate_all_autogates_variant = True,
        all_autogates_variant_excludes = [],
        **kwargs):
    """Defines a KJ test as `name@` and, unless generate_all_autogates_variant is False, as
    `name@all-autogates`, which runs with every autogate on but those named, without the
    `workerd-autogate-` prefix, in all_autogates_variant_excludes.

    Pass generate_all_autogates_variant = False for a test that sets the autogates itself, where
    the second variant would only repeat the first.
    """
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
        tags = tags,
        target_compatible_with = target_compatible_with,
        **kwargs
    )

    _kj_test(
        name = test_name + "@",
        binary = binary_name,
        data = data,
        size = size,
        shard_count = shard_count,
        **test_attrs(tags, size, target_compatible_with)
    )

    if not generate_all_autogates_variant:
        return

    _kj_test(
        name = test_name + "@all-autogates",
        binary = binary_name,
        data = data,
        env = {"WORKERD_ALL_AUTOGATES": "1"} | all_autogates_except_env(all_autogates_variant_excludes),
        # Tag with no-coverage to reduce coverage CI time
        size = size,
        shard_count = shard_count,
        **test_attrs(tags + ["no-coverage"], size, target_compatible_with)
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
