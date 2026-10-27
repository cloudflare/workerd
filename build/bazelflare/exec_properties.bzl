"""exec_properties that pick a target's executor pool on bazelflare (see BUILD.bazel)."""

def test_exec_properties(size, tags = []):
    """The exec_properties of a test of this size and these tags.

    The platform packs four actions onto each four-vCPU executor. Large and enormous tests get one
    to themselves, as do tests that ask for more than one CPU (a `cpu:` tag) and tests that hold a
    local resource (a `resources:` tag), which Bazel accounts for only on local execution: two tests
    that listen on the same fixed port could otherwise share an executor's network. The `test.` prefix applies to the test action alone, leaving the target's
    build actions on the shared executors. Other remote executors don't know the property, so only
    bazelflare's platforms take it.
    """
    if size in ("large", "enormous") or [t for t in tags if t.startswith("cpu:") or t.startswith("resources:")]:
        return select({
            Label("//build/bazelflare:on_bazelflare"): {"test.slots": "1"},
            "//conditions:default": {},
        })
    return {}
