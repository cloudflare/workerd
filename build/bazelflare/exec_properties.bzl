"""exec_properties that pick a target's executor pool on bazelflare (see BUILD.bazel)."""

def test_exec_properties(size, tags = []):
    """The exec_properties of a test of this size and these tags.

    The platform packs four actions onto each four-vCPU executor. Large and enormous tests get one
    to themselves, as do tests that ask for more than one CPU (a `cpu:` tag) and tests that hold a
    local resource (a `resources:` tag), which Bazel accounts for only on local execution: two tests
    that listen on the same fixed port could otherwise share an executor's network. Tests that reach
    the internet (`requires-network`) run on executors with egress, and tests that drive a Docker
    engine (`requires-container-engine`) get one of their own, on the deployment's image that runs it
    (bazelflare-workerd's executor/Dockerfile.dind). The `test.` prefix applies to the test action
    alone, leaving the target's build actions on the shared executors. Other remote executors don't
    know the properties, so only bazelflare's platforms take them.
    """
    props = {}
    if size in ("large", "enormous") or [t for t in tags if t.startswith("cpu:") or t.startswith("resources:")]:
        props["test.slots"] = "1"
    if "requires-network" in tags:
        props["test.network"] = "on"
    if "requires-container-engine" in tags:
        props["test.container-image"] = "executor-dind"
        props["test.slots"] = "1"
    if not props:
        return {}
    return select({
        Label("//build/bazelflare:on_bazelflare"): props,
        "//conditions:default": {},
    })
