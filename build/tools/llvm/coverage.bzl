"""The LLVM coverage tools for a test rule that runs a C++ binary, as rules_cc gives them to cc_test."""

COVERAGE_LLVM_TOOLS_ATTR = {
    "_coverage_llvm_tools": attr.label(
        default = "//build/tools/llvm:coverage",
        cfg = config.exec(exec_group = "test"),
    ),
}

_ENV_VARS = {"llvm-cov": "LLVM_COV", "llvm-profdata": "LLVM_PROFDATA"}

def coverage_llvm_tools(ctx, executable, environment):
    """Points Bazel's coverage collector at the tools, and returns the runfiles that carry them.

    The collector runs from the execroot, but a remote executor stages runfiles only in the test's
    runfiles tree, so the paths go through that tree.

    Args:
      ctx: the test rule's context, whose attrs include COVERAGE_LLVM_TOOLS_ATTR.
      executable: the test's executable.
      environment: the test's environment, which gains LLVM_COV and LLVM_PROFDATA.

    Returns:
      The tools' runfiles, empty unless coverage is enabled.
    """
    if not ctx.configuration.coverage_enabled:
        return ctx.runfiles()
    for tool in ctx.files._coverage_llvm_tools:
        if tool.short_path.startswith("../"):
            path = tool.short_path.removeprefix("../")
        else:
            path = ctx.workspace_name + "/" + tool.short_path
        environment[_ENV_VARS[tool.basename]] = executable.path + ".runfiles/" + path
    return ctx.runfiles(files = ctx.files._coverage_llvm_tools)
