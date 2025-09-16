"""Clang tidy aspect.

The aspect, when enabled runs clang_tidy on every compiled c++ file.
"""

load("@rules_cc//cc:action_names.bzl", "ACTION_NAMES")
load("@rules_cc//cc:find_cc_toolchain.bzl", "find_cc_toolchain")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")

def _clang_tidy_aspect_impl(target, ctx):
    # not a c++ target
    if not CcInfo in target:
        return []

    cc_toolchain = find_cc_toolchain(ctx)
    feature_configuration = cc_common.configure_features(
        ctx = ctx,
        cc_toolchain = cc_toolchain,
    )
    compile_variables = cc_common.create_compile_variables(
        feature_configuration = feature_configuration,
        cc_toolchain = cc_toolchain,
        user_compile_flags = ctx.fragments.cpp.cxxopts + ctx.fragments.cpp.copts,
    )
    toolchain_flags = cc_common.get_memory_inefficient_command_line(
        feature_configuration = feature_configuration,
        action_name = ACTION_NAMES.cpp_compile,
        variables = compile_variables,
    )

    compilation_context = target[CcInfo].compilation_context

    rule_copts = getattr(ctx.rule.attr, "copts", [])

    # we use $location in our copts, expand it
    rule_copts = [ctx.expand_location(opt) for opt in rule_copts]

    srcs = []
    if hasattr(ctx.rule.attr, "srcs"):
        for src in ctx.rule.attr.srcs:
            srcs += [
                src
                for src in src.files.to_list()
                if src.is_source and src.short_path.endswith((".c++", ".c", ".h"))
            ]
    if hasattr(ctx.rule.attr, "hdrs"):
        for src in ctx.rule.attr.hdrs:
            srcs += [
                src
                for src in src.files.to_list()
                if src.is_source and src.short_path.endswith((".c++", ".c", ".h"))
            ]

    defines = compilation_context.defines.to_list()
    local_defines = compilation_context.local_defines.to_list()
    includes = compilation_context.includes.to_list()
    quote_includes = compilation_context.quote_includes.to_list()
    system_includes = compilation_context.system_includes.to_list()
    headers = compilation_context.headers

    # disable clang tidy if no-clang-tidy tag is defined.
    # todo: figure out a better way to control clang tidy on a per-target basis.
    if "no-clang-tidy" in ctx.rule.attr.tags:
        return []

    # bazel doesn't expose implementation deps through compilation context
    # https://github.com/bazelbuild/bazel/issues/19663
    if hasattr(ctx.rule.attr, "implementation_deps"):
        deps = [dep[CcInfo].compilation_context for dep in ctx.rule.attr.implementation_deps if CcInfo in dep]
        defines = depset(
            defines,
            transitive = [dep.defines for dep in deps],
        )
        includes = depset(
            includes,
            transitive = [dep.includes for dep in deps],
        )
        system_includes = depset(
            system_includes,
            transitive = [dep.system_includes for dep in deps],
        )
        quote_includes = depset(
            quote_includes,
            transitive = [dep.quote_includes for dep in deps],
        )
        headers = depset(
            headers.to_list(),
            transitive = [dep.headers for dep in deps],
        )

    tools = [
        ctx.attr._clang_tidy_executable.files,
        ctx.attr._clang_tidy_wrapper.files,
        ctx.attr._clang_tidy_config.files,
    ]

    outs = []
    for src in srcs:
        # run actions need to produce something, declare a dummy file
        # multiple labels can use the same path, so disambiguate.
        out = ctx.actions.declare_file(src.path + "." + ctx.label.name + ".clang_tidy")
        outs.append(out)

        args = ctx.actions.args()

        # these are consumed by clang_tidy_wrapper,sh
        args.add(ctx.attr._clang_tidy_executable.files_to_run.executable)
        args.add(out)

        # clang-tidy arguments
        # do not print statistics
        args.add("--quiet")
        args.add("--config-file=" + ctx.attr._clang_tidy_config.files.to_list()[0].short_path)

        if ctx.attr.clang_tidy_args:
            args.add_all(ctx.attr.clang_tidy_args.split(" "))

        args.add(src.path)

        # compiler arguments
        args.add("--")

        args.add("-xc++")

        args.add_all(ctx.attr._clang_tidy_compiler_flags)
        args.add_all(rule_copts)
        args.add_all(defines, before_each = "-D")
        args.add_all(local_defines, before_each = "-D")
        args.add_all(includes, before_each = "-I")
        args.add_all(quote_includes, before_each = "-iquote")
        args.add_all(system_includes, before_each = "-isystem")

        args.add_all(toolchain_flags)

        # Silence warnings about unused functions or #pragma once being present in header files. For
        # source files, we already cover these warnings in regular compilation
        args.add("-Wno-pragma-once-outside-header")
        args.add("-Wno-unused")

        # TODO(cleanup): These paths provide required includes, but if the toolchain was working
        # properly we wouldn't need them in the first place...
        # Linux includes
        args.add("-isystem/usr/lib/llvm-19/include/c++/v1")
        args.add("-isystem/usr/lib/llvm-19/lib/clang/19/include")
        args.add("-isystem/usr/include")
        args.add("-isystem/usr/include/x86_64-linux-gnu")

        # macOS includes
        args.add("-isystem/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/usr/include/c++/v1")
        args.add("-isystem/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/lib/clang/17/include")
        args.add("-isystem/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/usr/include")

        inputs = depset(
            direct = [src],
            transitive = [headers],
        )

        ctx.actions.run(
            outputs = [out],
            arguments = [args],
            executable = ctx.attr._clang_tidy_wrapper.files_to_run.executable,
            progress_message = "Run clang-tidy on {}".format(src.short_path),
            tools = tools,
            mnemonic = "ClangTidy",
            inputs = inputs,
        )

    return [
        OutputGroupInfo(clang_tidy_checks = depset(direct = outs)),
    ]

clang_tidy_aspect = aspect(
    implementation = _clang_tidy_aspect_impl,
    fragments = ["cpp"],
    attrs = {
        "_clang_tidy_wrapper": attr.label(
            default = Label("@//build/tools/clang_tidy:clang_tidy_wrapper.sh"),
            allow_single_file = True,
        ),
        "_clang_tidy_executable": attr.label(
            default = Label("//tools:clang-tidy"),
            allow_single_file = True,
        ),
        "_clang_tidy_config": attr.label(
            default = Label("//:clang_tidy_config"),
            allow_single_file = True,
        ),
        "_clang_tidy_compiler_flags": attr.string_list(
            default = [],
        ),
        "clang_tidy_args": attr.string(default = ""),
    },
    toolchains = ["@bazel_tools//tools/cpp:toolchain_type"],
)
