"""wd_cc_binary definition"""

load("@rules_cc//cc:cc_binary.bzl", "cc_binary")

def wd_cc_binary(
        name,
        visibility = None,
        deps = [],
        **kwargs):
    """Wrapper for cc_binary that sets common attributes
    """
    cc_binary(
        name = name,
        visibility = visibility,
        deps = deps + ["//build/deps:linkopts_tool"],
        **kwargs
    )
