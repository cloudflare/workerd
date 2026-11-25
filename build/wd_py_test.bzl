"""py_test with the attributes that workerd's test rules derive from test tags."""

load("@rules_python//python:defs.bzl", "py_test")
load("//:build/test_attrs.bzl", "test_attrs")

def wd_py_test(name, tags = [], size = "medium", target_compatible_with = None, exec_properties = {}, **kwargs):
    """py_test, with tags, exec_properties and target_compatible_with completed by test_attrs().

    Args:
        name: the test's name.
        tags: the test's tags.
        size: the test's size.
        target_compatible_with: the test's own target_compatible_with, if any.
        exec_properties: the test's own exec_properties, if any.
        **kwargs: the other py_test attributes.
    """
    py_test(
        name = name,
        size = size,
        **(kwargs | test_attrs(tags, size, target_compatible_with, exec_properties))
    )
