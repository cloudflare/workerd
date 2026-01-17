import pytest
from js import setTimeout
from workers import import_from_javascript

from pyodide import __version__
from pyodide.ffi import create_once_callable


def test_import_from_javascript():
    # Import the env module from cloudflare:workers
    env_module = import_from_javascript("cloudflare:workers")

    # Test that we can access the imported module's exports
    assert hasattr(env_module, "env"), "env property should exist"
    assert hasattr(env_module, "withEnv"), "withEnv function should exist"

    # Test with actual env values
    assert env_module.env.TEST_VALUE == "TEST_STRING", (
        "TEST_VALUE should be correctly set"
    )

    # Test sockets module
    sockets_module = import_from_javascript("cloudflare:sockets")
    assert hasattr(sockets_module, "connect"), (
        "sockets_module should have 'connect' attribute"
    )

    def f():
        pass

    setTimeout(create_once_callable(f), 1)


@pytest.mark.skipif(__version__ == "0.26.0a2", reason="Requires JSPI")
def test_import_vectorize():
    # Assert that imports that depend on JSPI work as expected
    vectorize = import_from_javascript("cloudflare:vectorize")
    assert hasattr(vectorize, "DistanceMetric")


def test_import_failures():
    # Try importing an unsafe module which should fail
    with pytest.raises(ImportError):
        import_from_javascript("internal:unsafe-eval")

    # Assert that non existent modules throw ImportError
    with pytest.raises(ImportError):
        import_from_javascript("crypto")
