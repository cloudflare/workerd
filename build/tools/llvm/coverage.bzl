"""LLVM coverage tools for a test rule that runs a C++ binary, as rules_cc gives them to cc_test.

They come from an optional toolchain, which workerd registers in build/deps/llvm.MODULE.bazel.
A build that loads workerd without its MODULE.bazel registers none, so its collector finds the
tools on PATH, as on macOS.
"""

_TOOLCHAIN_TYPE = Label("//build/tools/llvm:coverage_toolchain_type")

COVERAGE_TOOLCHAINS = [
    config_common.toolchain_type(_TOOLCHAIN_TYPE, mandatory = False),
]

def _llvm_coverage_toolchain_impl(ctx):
    return [platform_common.ToolchainInfo(
        llvm_cov = ctx.file.llvm_cov,
        llvm_profdata = ctx.file.llvm_profdata,
    )]

llvm_coverage_toolchain = rule(
    implementation = _llvm_coverage_toolchain_impl,
    attrs = {
        "llvm_cov": attr.label(allow_single_file = True, cfg = "exec", mandatory = True),
        "llvm_profdata": attr.label(allow_single_file = True, cfg = "exec", mandatory = True),
    },
)

def coverage_llvm_tools(ctx, executable, environment):
    """Points Bazel's coverage collector at the tools, and returns the runfiles that carry them.

    The collector runs from the execroot, but a remote executor stages runfiles only in the test's
    runfiles tree, so the paths go through that tree.

    Args:
      ctx: the test rule's context, whose rule takes COVERAGE_TOOLCHAINS.
      executable: the test's executable.
      environment: the test's environment, which gains LLVM_COV and LLVM_PROFDATA.

    Returns:
      The tools' runfiles, empty unless coverage is enabled and a toolchain resolved.
    """
    toolchain = ctx.toolchains[_TOOLCHAIN_TYPE]
    if not ctx.configuration.coverage_enabled or not toolchain:
        return ctx.runfiles()
    tools = {"LLVM_COV": toolchain.llvm_cov, "LLVM_PROFDATA": toolchain.llvm_profdata}
    for name, tool in tools.items():
        if tool.short_path.startswith("../"):
            path = tool.short_path.removeprefix("../")
        else:
            path = ctx.workspace_name + "/" + tool.short_path
        environment[name] = executable.path + ".runfiles/" + path
    return ctx.runfiles(files = tools.values())
