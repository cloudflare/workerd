import pytest
from js import Date, Reflect
from workers import env, import_from_javascript, patch_env

from pyodide import __version__
from pyodide.ffi import create_proxy, to_js

if __version__ == "0.26.0a2":
    pytest.skip("Not supported on 0.26.0a2", allow_module_level=True)


@pytest.mark.asyncio
async def test_memory_cache():
    # Test that cache exists and is accessible
    assert env.CACHE, "env.CACHE should exist"

    # Test accessing the cache
    async def memory_cache_read(x):
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


@pytest.mark.asyncio
async def test_with_env():
    workers_module = import_from_javascript("cloudflare:workers")

    # Test withEnv
    async def import_with_env():
        workers_module = import_from_javascript("cloudflare:workers")
        env = workers_module.env
        return env.FOO

    result = await workers_module.withEnv(to_js({"FOO": 1}), import_with_env)
    assert result == 1


def check_normal():
    assert env.secret == "thisisasecret"
    assert not hasattr(env, "NAME")
    assert not hasattr(env, "place")
    assert not hasattr(env, "extra")
    assert set(Reflect.ownKeys(env).to_py()).issuperset({"secret"})


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
