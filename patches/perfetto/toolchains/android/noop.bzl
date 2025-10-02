def _noop_toolchain_impl(ctx):
    toolchain_info = platform_common.ToolchainInfo(

    )
    return [toolchain_info]

noop_toolchain = rule(
    implementation = _noop_toolchain_impl,
    attrs = {

    },
)
