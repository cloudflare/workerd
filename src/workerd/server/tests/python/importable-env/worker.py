from workers import import_from_javascript

from pyodide import __version__
from pyodide.ffi import create_proxy, to_js

# Then test the regular import function
workers_module = import_from_javascript("cloudflare:workers")
env = workers_module.env
assert env.FOO == "BAR", "env.FOO should be 'BAR'"
sockets_module = import_from_javascript("cloudflare:sockets")

# TODO: in 0.27.7 this ends up attempting to perform async IO in the global scope, this appears
# to be a bug in Pyodide which ends up calling setTimeout(cb, 0).
if __version__ != "0.27.7":
    try:
        import_from_javascript("cloudflare:vectorize")
        assert __version__ != "0.26.0a2", (
            'import_from_javascript("cloudflare:vectorize") should not work in the global scope in 0.26.0a2'
        )
    except ImportError:
        # This should throw "ImportError: Failed to import 'cloudflare:vectorize': Only 'cloudflare:workers' and 'cloudflare:sockets' are available until the next python runtime version."
        assert __version__ == "0.26.0a2"


async def test():
    # Test sockets module if available
    assert hasattr(sockets_module, "connect"), (
        "sockets_module should have 'connect' attribute"
    )

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
