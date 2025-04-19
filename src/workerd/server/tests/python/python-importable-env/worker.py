import asyncio

from workers import import_from_javascript

cloudflare_workers = import_from_javascript("cloudflare:workers")
env = cloudflare_workers.env


async def test():
    from js import console

    console.log("Running Python importable-env tests...")

    # Import assert functionality from Node.js
    node_assert = import_from_javascript("node:assert")

    # Import the env module from cloudflare:workers
    withEnv = cloudflare_workers.withEnv

    # Test that env is populated at the top level scope
    node_assert.strictEqual(env.FOO, "BAR")
    console.log("env.FOO correctly set to 'BAR'")

    # Test that cache exists and is accessible
    node_assert.ok(env.CACHE)
    console.log("env.CACHE exists")

    # Test accessing the cache
    current_time = await cloudflare_workers.scheduler.now()
    cached = await env.CACHE.read(
        "hello",
        lambda: {
            "value": 123,
            "expiration": current_time + 10000,
        },
    )
    node_assert.strictEqual(cached, 123)
    console.log("cache.read works correctly")

    # Test mutation of env
    env.BAR = 123
    child = import_from_javascript("child")
    node_assert.strictEqual(child.env.FOO, "BAR")
    node_assert.strictEqual(child.env.BAR, 123)
    console.log("env mutations are visible in imports")

    # Test withEnv
    async def import_with_env():
        await asyncio.sleep(0)
        return import_from_javascript("child2")

    result = await withEnv({"BAZ": 1}, import_with_env)
    child2_env = result.env

    # Child2 should have BAZ but not FOO
    assert not hasattr(child2_env, "FOO") or child2_env.FOO is None, (
        "child2_env.FOO should be None"
    )
    assert hasattr(child2_env, "BAZ") and child2_env.BAZ == 1, (
        "child2_env.BAZ should be 1"
    )

    # Original env should not have BAZ
    assert not hasattr(env, "BAZ") or env.BAZ is None, "env.BAZ should be None"
    console.log("withEnv works correctly")

    console.log("All Python importable-env tests passed!")
