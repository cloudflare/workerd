"""Attributes that workerd's test rules derive from a test's tags."""

load("//build/bazelflare:exec_properties.bzl", "test_exec_properties")

def test_attrs(tags = [], size = "medium", target_compatible_with = None, exec_properties = {}):
    """Returns the attributes of a test that follow from its tags and size.

    Every test rule of workerd's passes its tests' attributes through this, so that a tag means the
    same for each of them.

    Args:
        tags: the test's tags.
        size: the test's size.
        target_compatible_with: the test's own target_compatible_with, if any.
        exec_properties: the test's own exec_properties, which take precedence.

    Returns:
        A dict with the test's tags, its exec_properties, with those that bazelflare needs
        (test_exec_properties), and its target_compatible_with.
    """
    if target_compatible_with == None:
        target_compatible_with = []
    return {
        "exec_properties": test_exec_properties(size, tags) | exec_properties,
        "tags": tags,
        "target_compatible_with": target_compatible_with,
    }
