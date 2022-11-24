"""
Dump the AST of a given C++ source file

Based loosely on https://github.com/bazelbuild/rules_cc/blob/main/examples/my_c_compile/my_c_compile.bzl
"""

load("@bazel_tools//tools/cpp:toolchain_utils.bzl", "find_cpp_toolchain", "use_cpp_toolchain")
load("@rules_cc//cc:action_names.bzl", "CPP_COMPILE_ACTION_NAME")

def _cc_ast_dump_impl(ctx):
    cc_toolchain = find_cpp_toolchain(ctx)

    feature_configuration = cc_common.configure_features(
        ctx = ctx,
        cc_toolchain = cc_toolchain,
        requested_features = ctx.features,
        unsupported_features = ctx.disabled_features,
    )

    cc_info = cc_common.merge_cc_infos(direct_cc_infos = [dep[CcInfo] for dep in ctx.attr.deps])

    variables = cc_common.create_compile_variables(
        cc_toolchain = cc_toolchain,
        feature_configuration = feature_configuration,
        source_file = ctx.file.src.path,
        output_file = None,
        user_compile_flags = ["-Xclang", "-ast-dump=json", "-fsyntax-only"] + ctx.fragments.cpp.copts + ctx.fragments.cpp.cxxopts,
        include_directories = cc_info.compilation_context.includes,
        quote_include_directories = cc_info.compilation_context.quote_includes,
        system_include_directories = cc_info.compilation_context.system_includes,
        framework_include_directories = cc_info.compilation_context.framework_includes,
        preprocessor_defines = cc_info.compilation_context.defines,
    )

    arguments = cc_common.get_memory_inefficient_command_line(feature_configuration = feature_configuration, action_name = CPP_COMPILE_ACTION_NAME, variables = variables)
    executable = cc_common.get_tool_for_action(feature_configuration = feature_configuration, action_name = CPP_COMPILE_ACTION_NAME)
    inputs = depset(direct = [ctx.file.src], transitive = [cc_toolchain.all_files] + [cc_info.compilation_context.headers])
    env = cc_common.get_environment_variables(feature_configuration = feature_configuration, action_name = CPP_COMPILE_ACTION_NAME, variables = variables)

    command = " ".join([executable] + arguments + [">", ctx.outputs.out.path])

    # run_shell until https://github.com/bazelbuild/bazel/issues/5511 is fixed
    ctx.actions.run_shell(
        outputs = [ctx.outputs.out],
        inputs = inputs,
        command = command,
        env = env,
    )

cc_ast_dump = rule(
    implementation = _cc_ast_dump_impl,
    attrs = {
        "src": attr.label(mandatory = True, allow_single_file = True),
        "out": attr.output(mandatory = True),
        "deps": attr.label_list(providers = [CcInfo]),
        "_cc_toolchain": attr.label(default = Label("@bazel_tools//tools/cpp:current_cc_toolchain")),
    },
    toolchains = use_cpp_toolchain(),  # copybara-use-repo-external-label
    fragments = ["cpp"],
)
