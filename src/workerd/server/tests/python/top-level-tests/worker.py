# Check that multiprocessing top level import works
import multiprocessing

from workers import WorkerEntrypoint, import_from_javascript

from pyodide import __version__


def use(x):
    pass


def f():
    pass


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
        assert str(err).startswith(
            "Error: Disallowed operation called within global scope"
        )
        del err
    return [workers_module, env, sockets_module, vectorize]


# Check serialization of modules
TOP_LEVEL = top_level_test()


class Default(WorkerEntrypoint):
    async def test(self):
        use(multiprocessing)
