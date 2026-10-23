"""wd_cc_binary definition"""

load("@rules_cc//cc:cc_binary.bzl", "cc_binary")

def wd_cc_binary(
        name,
        visibility = None,
        deps = [],
        tags = [],
        **kwargs):
    """Wrapper for cc_binary that sets common attributes
    """
    cc_binary(
        name = name,
        visibility = visibility,
        # Tag with cpu:4 since this target depends on linkopts_tool.
        tags = tags + ["cpu:4"],
        # Use dedicated high-CPU runner if available
        exec_properties = {"runner": "high-cpu"},
        deps = deps + ["//build/deps:linkopts_tool"],
        **kwargs
    )
