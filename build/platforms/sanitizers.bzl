"""The `no-asan` and `no-tsan` test tags.

A test tagged `no-asan` (`no-tsan`) is incompatible with --config=asan (--config=tsan): wildcard
patterns such as `//...` skip it, and naming it explicitly fails. Tag filters can't do this, since
--test_tag_filters takes a single value, so a sanitizer config that set one would replace another
config's, such as bazelflare's.

wd_test, kj_test and wd_rust_test apply the tags. Other tests load sh_test or py_test from here,
or add `sanitizer_compatible_with(tags)` to target_compatible_with.
"""

load("@rules_python//python:defs.bzl", _py_test = "py_test")
load("@rules_shell//shell:sh_test.bzl", _sh_test = "sh_test")

_INCOMPATIBLE_WITH = {
    "no-asan": select({
        Label("//build/platforms:sanitizer_address"): ["@platforms//:incompatible"],
        "//conditions:default": [],
    }),
    "no-tsan": select({
        Label("//build/platforms:sanitizer_thread"): ["@platforms//:incompatible"],
        "//conditions:default": [],
    }),
}

def sanitizer_compatible_with(tags, target_compatible_with = None):
    """Adds to target_compatible_with the sanitizers that tags exclude.

    Args:
        tags: the target's tags.
        target_compatible_with: the target's own target_compatible_with, a list or a select.

    Returns:
        The combined target_compatible_with.
    """
    result = target_compatible_with or []
    for tag, incompatible in _INCOMPATIBLE_WITH.items():
        if tag in tags:
            result = result + incompatible
    return result

def sh_test(name, tags = [], target_compatible_with = None, **kwargs):
    """sh_test that applies the sanitizer tags."""
    _sh_test(
        name = name,
        tags = tags,
        target_compatible_with = sanitizer_compatible_with(tags, target_compatible_with),
        **kwargs
    )

def py_test(name, tags = [], target_compatible_with = None, **kwargs):
    """py_test that applies the sanitizer tags."""
    _py_test(
        name = name,
        tags = tags,
        target_compatible_with = sanitizer_compatible_with(tags, target_compatible_with),
        **kwargs
    )
