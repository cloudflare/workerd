"""wd_cc_binary definition"""

def wd_cc_binary(
        name,
        linkopts = [],
        visibility = None,
        **kwargs):
    """Wrapper for cc_binary that sets common attributes
    """
    native.cc_binary(
        name = name,
        linkopts = linkopts + select({
          "@//:use_dead_strip": ["-Wl,-dead_strip"],
          "//conditions:default": [""],
        }),
        visibility = visibility,
        **kwargs
    )
