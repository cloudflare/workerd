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

def link_exec_properties():
    """exec_properties for select binaries that we want to link quickly to keep the critical path
    short.
    Linking large binaries can be slow, especially since lld runs optimizations such as -Wl,-O2 in
    release builds, so we should use a larger runner: a whole executor for bazelflare and a high-CPU
    one for internal RBE. The `cpp_link.` prefix is used so this is limited to the cpp_link exec
    group (only for internal builds as rules_rust doesn't use the cpp_link group by default).
    """
    return select({
        Label("//build/config:link_on_bazelflare"): {"slots": "1"},
        Label("//build/config:link_on_bazelflare_internal"): {"cpp_link.slots": "1"},
        Label("//build/config:link_on_rbe_ci"): {"cpp_link.runner": "high-cpu"},
        "//conditions:default": {},
    })
