"""exec_properties that pick a target's executor pool on bazelflare (see BUILD.bazel)."""

# The vCPUs of the platform's executors, which it packs with one action per vCPU.
_EXECUTOR_VCPUS = 4

def test_exec_properties(size, tags = []):
    """The exec_properties of a test with these tags.

    The platform packs four actions onto each four-vCPU executor. A test that asks for n CPUs (a
    `cpu:n` tag) runs on executors that pack 4/n actions, or gets one to itself if n is more than
    two. Tests that hold a local resource (a `resources:` tag), which Bazel accounts for only on
    local execution, get one to themselves too: two tests that listen on the same fixed port could
    otherwise share an executor's network. Tests that reach the internet (`requires-network`) run
    on executors with egress, and tests that drive a Docker engine (`requires-container-engine`)
    get one of their own, on the deployment's image that runs it (bazelflare-workerd's
    executor/Dockerfile.dind). The `test.` prefix applies to the test action alone, leaving the
    target's build actions on the shared executors. Other remote executors don't know the
    properties, so only bazelflare's platforms take them.

    A test's size picks only its timeout, not its executor: Bazel counts a test of any size as one
    CPU. `size` stays a parameter so that callers in other repositories keep working.
    """
    props = {}
    cpus = max([int(t.removeprefix("cpu:")) for t in tags if t.startswith("cpu:")] + [1])
    slots = max(1, _EXECUTOR_VCPUS // cpus)
    if [t for t in tags if t.startswith("resources:")]:
        slots = 1
    if "requires-network" in tags:
        props["test.network"] = "on"
    if "requires-container-engine" in tags:
        props["test.container-image"] = "executor-dind"
        slots = 1
    if slots < _EXECUTOR_VCPUS:
        props["test.slots"] = str(slots)
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
