from js import Reflect
from workers import WorkerEntrypoint, env, patch_env, import_from_javascript

def top_level_test():
    # Test the regular import function
    workers_module = import_from_javascript("cloudflare:workers")
    env = workers_module.env
    assert env.FOO == "BAR", "env.FOO should be 'BAR'"
    sockets_module = import_from_javascript("cloudflare:sockets")

    vectorize = None
    threw = False
    try:
        vectorize = import_from_javascript("cloudflare:vectorize")
    except ImportError:
        threw = True


    if __version__ == "0.26.0a2":
        # Should have thrown throw "ImportError: Failed to import 'cloudflare:vectorize': Only 'cloudflare:workers' and 'cloudflare:sockets' are available until the next python runtime version."
        msg = 'import_from_javascript("cloudflare:vectorize") not expected to work in the global scope in 0.26.0a2'
        assert threw, msg
        assert vectorize is None, msg
    else:
        assert {"DistanceMetric", "KnownModel", "MetadataRetrievalLevel"}.issubset(
            dir(vectorize)
        )

    from js import setTimeout

    from pyodide.ffi import create_once_callable


    def f():
        pass


    # Checks for the patch to top level setTimeout() that is needed for top level
    # `import_from_javascript``.
    # 1. Test that setTimeout with a zero timeout works at top level
    setTimeout(create_once_callable(f), 0)

    # 2. Test that setTimeout with nonzero timeout fails with the expected error.
    err = None
    try:
        setTimeout(create_once_callable(f), 1)
    except Exception as e:
        err = e
    finally:
        assert err
        assert str(err).startswith("Error: Disallowed operation called within global scope")
        del err
    return [workers_module, env, sockets_module, vectorize]


# Check serialization of modules
TOP_LEVEL = top_level_test()


def check_normal():
    assert env.secret == "thisisasecret"
    assert not hasattr(env, "NAME")
    assert not hasattr(env, "place")
    assert not hasattr(env, "extra")
    assert Reflect.ownKeys(env).to_py() == ["secret"]

def test_import_from_javascript1();
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


def test_import_from_javascript2():
    # Test sockets module if available
    assert hasattr(sockets_module, "connect"), (
        "sockets_module should have 'connect' attribute"
    )
    setTimeout(create_once_callable(f), 1)

    # Test that cache exists and is accessible
    assert env.CACHE, "env.CACHE should exist"

    # Assert that non existent modules throw ImportError
    try:
        import_from_javascript("crypto")
        raise RuntimeError('import_from_javascript("crypto") should not work')
    except ImportError:
        pass

    if __version__ != "0.26.0a2":
        # to_js doesn't work correctly in 0.26.0a2 for some reason
        # Test accessing the cache
        async def memory_cache_read(x):
            from js import Date

            return to_js(
                {
                    "value": 123,
                    "expiration": Date.now() + 10000,
                }
            )

        cached = await env.CACHE.read(
            "hello",
            create_proxy(memory_cache_read),
        )
        assert cached == 123, "cached value should be 123"

        # Test withEnv
        async def import_with_env():
            workers_module = import_from_javascript("cloudflare:workers")
            env = workers_module.env
            return env.FOO

        result = await workers_module.withEnv(to_js({"FOO": 1}), import_with_env)
        assert result == 1

    if __version__ == "0.26.0a2":
        try:
            vectorize = import_from_javascript("cloudflare:vectorize")
        except ImportError as e:
            assert (
                e.args[0]
                == "Failed to import 'cloudflare:vectorize': Only 'cloudflare:workers' and 'cloudflare:sockets' are available until the next python runtime version."
            )
    else:
        # Assert that imports that depend on JSPI work as expected
        vectorize = import_from_javascript("cloudflare:vectorize")
        assert hasattr(vectorize, "DistanceMetric")


def test_patch_env():
    check_normal()

    with patch_env(NAME=1, place="Somewhere"):
        assert env.NAME == 1
        assert env.place == "Somewhere"
        assert not hasattr(env, "secret")

    check_normal()

    with patch_env({"NAME": 1, "place": "Somewhere"}):
        assert env.NAME == 1
        assert env.place == "Somewhere"
        assert not hasattr(env, "secret")

    check_normal()

    with patch_env([("NAME", 1), ("place", "Somewhere")]):
        assert env.NAME == 1
        assert env.place == "Somewhere"
        assert not hasattr(env, "secret")

    check_normal()

    with patch_env({"NAME": 1, "place": "Somewhere"}, extra=22):
        assert env.NAME == 1
        assert env.place == "Somewhere"
        assert env.extra == 22
        assert not hasattr(env, "secret")

    check_normal()

    try:
        with patch_env(NAME=1, place="Somewhere"):
            raise RuntimeError("oops")  # noqa: TRY301
    except Exception:
        pass

    check_normal()


class Default(WorkerEntrypoint):
    def test(self):
        test_import_from_javascript1()
        test_import_from_javascript2()
        test_patch_env()
