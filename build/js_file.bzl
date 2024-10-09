"""
Give a collection of js files a JsInfo provider so it can be used as a dependency for aspect_rules.
"""

load("@aspect_bazel_lib//lib:copy_to_bin.bzl", "COPY_FILE_TO_BIN_TOOLCHAINS", "copy_file_to_bin_action")
load("@aspect_rules_js//js:providers.bzl", "JsInfo", "js_info")

_ATTRS = {
    "srcs": attr.label_list(
        allow_files = True,
    ),
    "deps": attr.label_list(),
}

def _gather_sources_and_types(ctx, targets, files):
    """Gathers sources and types from a list of targets

    Args:
        ctx: the rule context

        targets: List of targets to gather sources and types from their JsInfo providers.

            These typically come from the `srcs` and/or `data` attributes of a rule

        files: List of files to gather as sources and types.

            These typically come from the `srcs` and/or `data` attributes of a rule

    Returns:
        Sources & declaration files depsets in the sequence (sources, types)
    """
    sources = []
    types = []

    for file in files:
        if file.is_source:
            file = copy_file_to_bin_action(ctx, file)

        if file.is_directory:
            # assume a directory contains types since we can't know that it doesn't
            types.append(file)
            sources.append(file)
        elif (
            file.path.endswith((".d.ts", ".d.ts.map", ".d.mts", ".d.mts.map", ".d.cts", ".d.cts.map"))
        ):
            types.append(file)
        elif file.path.endswith(".json"):
            # Any .json can produce types: https://www.typescriptlang.org/tsconfig/#resolveJsonModule
            # package.json may be required to resolve types with the "typings" key
            types.append(file)
            sources.append(file)
        else:
            sources.append(file)

    # sources as depset
    sources = depset(sources, transitive = [
        target[JsInfo].sources
        for target in targets
        if JsInfo in target
    ])

    # types as depset
    types = depset(types, transitive = [
        target[JsInfo].types
        for target in targets
        if JsInfo in target
    ])

    return (sources, types)

def _js_file_impl(ctx):
    sources, types = _gather_sources_and_types(
        ctx = ctx,
        targets = ctx.attr.srcs,
        files = ctx.files.srcs,
    )

    return [
        js_info(
            target = ctx.label,
            sources = sources,
            types = types,
        ),
        DefaultInfo(
            files = sources,
        ),
        OutputGroupInfo(
            types = types,
        ),
    ]

js_file_lib = struct(
    attrs = _ATTRS,
    implementation = _js_file_impl,
    provides = [DefaultInfo, JsInfo, OutputGroupInfo],
)

js_file = rule(
    implementation = js_file_lib.implementation,
    attrs = js_file_lib.attrs,
    provides = js_file_lib.provides,
    toolchains = COPY_FILE_TO_BIN_TOOLCHAINS,
)
