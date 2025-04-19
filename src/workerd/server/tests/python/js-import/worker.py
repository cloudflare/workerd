from workers import import_from_javascript


async def test():
    # Import the env module from cloudflare:workers
    env_module = import_from_javascript("cloudflare:workers")

    # Test that we can access the imported module's exports
    assert hasattr(env_module, "env"), "env property should exist"
    assert hasattr(env_module, "withEnv"), "withEnv function should exist"

    # Test with actual env values
    assert env_module.env.TEST_VALUE == "TEST_STRING", (
        "TEST_VALUE should be correctly set"
    )

    # Try importing an unsafe module which should fail
    try:
        import_from_javascript("internal:unsafe-eval")
        raise AssertionError("Unsafe import should have failed but succeeded")
    except ImportError:
        # This is expected to fail, so the test passes
        pass
