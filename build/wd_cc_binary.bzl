"""wd_cc_binary definition"""

def wd_cc_binary(
        name,
        visibility = None,
        **kwargs):
    """Wrapper for cc_binary that sets common attributes
    """
    native.cc_binary(
        name = name,
        visibility = visibility,
        **kwargs
    )
