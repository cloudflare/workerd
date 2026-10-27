# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

"""The registered Python toolchain's interpreter, as a file a non-Python rule can run.

Rules that run Python from a shell, such as rules_foreign_cc's cmake() configuring a project that
needs Python, can't take the toolchain themselves. Listed as an execution-platform dependency (in
cmake()'s build_data, say), this gives them the interpreter for the platform their actions run on,
with the rest of the runtime in its runfiles, so `$(execpath)` names it.
"""

def _python_interpreter_impl(ctx):
    runtime = ctx.toolchains["@rules_python//python:toolchain_type"].py3_runtime
    return [DefaultInfo(
        files = depset([runtime.interpreter]),
        runfiles = ctx.runfiles(transitive_files = runtime.files),
    )]

python_interpreter = rule(
    implementation = _python_interpreter_impl,
    toolchains = ["@rules_python//python:toolchain_type"],
)
