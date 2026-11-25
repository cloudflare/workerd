"""Attributes that workerd's test rules derive from a test's tags."""

load("//build/bazelflare:exec_properties.bzl", "test_exec_properties")

def tag_compatibility(tags):
    """Returns the target_compatible_with value that a test's tags call for.

    Tests tagged requires-container-engine are compatible only where the machine that runs them has
    a container engine: on bazelflare, whose executors have one, or with
    --//build/config:has_container_engine. Elsewhere `bazel test //...` skips them, and a test
    requested by name fails with an error naming //build/config:requires_container_engine. Unlike a
    test tag filter, which the last config that sets one replaces, this combines with every config.

    Args:
        tags: the test's tags.

    Returns:
        A list, or a select() of lists, to add to the test's target_compatible_with.
    """
    if "requires-container-engine" not in tags:
        return []
    return select({
        Label("//build/config:container_engine_available"): [],
        "//conditions:default": [Label("//build/config:requires_container_engine")],
    })

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
        (test_exec_properties), and its target_compatible_with, with tag_compatibility() added.
    """
    if target_compatible_with == None:
        target_compatible_with = []
    return {
        "exec_properties": test_exec_properties(size, tags) | exec_properties,
        "tags": tags,
        "target_compatible_with": target_compatible_with + tag_compatibility(tags),
    }
