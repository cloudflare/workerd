"""Expose real dependency headers to standalone clang-tidy fixture tests."""

load("@rules_cc//cc:action_names.bzl", "ACTION_NAMES")
load("@rules_cc//cc:find_cc_toolchain.bzl", "find_cc_toolchain", "use_cc_toolchain")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")

def _cc_test_headers_impl(ctx):
    return [DefaultInfo(files = ctx.attr.dep[CcInfo].compilation_context.headers)]

cc_test_headers = rule(
    implementation = _cc_test_headers_impl,
    attrs = {"dep": attr.label(mandatory = True, providers = [CcInfo])},
)

# The C++ toolchain's compile flags, one per line, which point clang-tidy at the toolchain's
# standard library, resource and sysroot headers, carried in runfiles. A test runs from its main
# repository's runfiles directory, where another repository's files are under ../<repository>
# rather than external/<repository>, so the paths are rewritten to match.
def _cc_test_toolchain_flags_impl(ctx):
    cc_toolchain = find_cc_toolchain(ctx)
    feature_configuration = cc_common.configure_features(
        ctx = ctx,
        cc_toolchain = cc_toolchain,
        requested_features = ctx.features,
        unsupported_features = ctx.disabled_features,
    )
    flags = cc_common.get_memory_inefficient_command_line(
        feature_configuration = feature_configuration,
        action_name = ACTION_NAMES.cpp_compile,
        variables = cc_common.create_compile_variables(
            feature_configuration = feature_configuration,
            cc_toolchain = cc_toolchain,
        ),
    )
    out = ctx.actions.declare_file(ctx.label.name + ".txt")
    ctx.actions.write(out, "".join([flag.replace("external/", "../") + "\n" for flag in flags]))
    return [DefaultInfo(
        files = depset([out]),
        runfiles = ctx.runfiles(transitive_files = cc_toolchain.all_files),
    )]

cc_test_toolchain_flags = rule(
    implementation = _cc_test_toolchain_flags_impl,
    fragments = ["cpp"],
    toolchains = use_cc_toolchain(),
)
