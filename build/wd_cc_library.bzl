"""wd_cc_library definition"""

def wd_cc_library(strip_include_prefix = "/src", **kwargs):
    """Wrapper for cc_library that sets common attributes
    """
    native.cc_library(
        strip_include_prefix = strip_include_prefix,
        **kwargs
    )
