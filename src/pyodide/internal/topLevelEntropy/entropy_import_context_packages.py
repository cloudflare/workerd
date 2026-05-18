"""
These are top level entropy patches for packages.

We'll need to keep these for backwards compatibility but our goal is to stop including these and
move them into workers-runtime-sdk.
"""

import sys
from contextlib import contextmanager

from .allow_entropy import (
    allow_bad_entropy_calls,
)
from .import_patch_manager import (
    block_calls,
    register_after_snapshot,
    register_before_first_request,
    register_create_patch,
    register_exec_patch,
)

IMPORTED_RUST_PACKAGE = False


@register_create_patch("tiktoken._tiktoken")
@register_exec_patch("cryptography.exceptions")
@register_exec_patch("jiter")
@contextmanager
def rust_package_context(module):
    """Rust packages need one entropy call if they create a rust hash map at
    init time.

    For reasons I don't entirely understand, in Pyodide 0.28 only the first Rust package to be
    imported makes the get_entropy call. See gen_rust_import_tests() which tests that importing
    four rust packages in different permutations works correctly.
    """
    global IMPORTED_RUST_PACKAGE
    if sys.version_info >= (3, 13) and IMPORTED_RUST_PACKAGE:
        yield
        return
    IMPORTED_RUST_PACKAGE = True
    with allow_bad_entropy_calls(1):
        yield


NUMPY_RANDOM_STATE = None


@register_exec_patch("numpy.random")
@contextmanager
def numpy_random_context(numpy_random):
    """numpy.random doesn't call getentropy() itself, but we want to block calls
    that might use the bad seed.

    TODO: Maybe there are more calls we can whitelist?
    TODO: Is it not enough to just block numpy.random.mtrand calls?
    """
    yield
    # Calling default_rng() with a given seed is fine, calling it without a seed
    # will call getentropy() and fail.
    block_calls(numpy_random, allowlist=("default_rng", "RandomState"))


@register_after_snapshot("numpy.random")
def numpy_random_after_snapshot(numpy_random):
    r1 = numpy_random.random()
    numpy_random.set_state(NUMPY_RANDOM_STATE)
    r2 = numpy_random.random()
    if r1 != r2:
        raise RuntimeError("random seed in bad state")


@register_before_first_request("numpy.random")
def numpy_random_before_first_request(numpy_random):
    numpy_random.seed()


@register_exec_patch("numpy.random.mtrand")
@contextmanager
def numpy_random_mtrand_context(module):
    # numpy.random.mtrand calls secrets.randbits at top level to seed itself.
    # This will fail if we don't let it through.
    with allow_bad_entropy_calls(1):
        yield
    # Block calls until we get a chance to replace the bad random seed.
    global NUMPY_RANDOM_STATE
    NUMPY_RANDOM_STATE = module.get_state()
    block_calls(module, allowlist=("RandomState",))


@register_exec_patch("pydantic_core")
@contextmanager
def pydantic_core_context(module):
    try:
        # Initial import needs one entropy call to initialize
        # std::collections::HashMap hash seed
        with allow_bad_entropy_calls(1):
            yield
    finally:
        try:
            with allow_bad_entropy_calls(1):
                # validate_core_schema makes an ahash::AHashMap which makes
                # another entropy call for its hash seed. It will throw an error
                # but only after making the needed entropy call.
                module.validate_core_schema(None)
        except module.SchemaError:
            pass


@register_exec_patch("aiohttp.http_websocket")
@contextmanager
def aiohttp_http_websocket_context(module):
    import random

    Random = random.Random

    def patched_Random():
        return random

    random.Random = patched_Random
    try:
        yield
    finally:
        random.Random = Random


class NoSslFinder:
    def find_spec(self, fullname, path, target):
        if fullname == "ssl":
            raise ModuleNotFoundError(
                f"No module named {fullname!r}", name=fullname
            ) from None


@contextmanager
def no_ssl():
    """
    Various packages will call ssl.create_default_context() at top level which uses entropy if they
    can import ssl. By temporarily making importing ssl raise an import error, we exercise the
    workaround code and so avoid the entropy calls. After, we put the ssl module back to the normal
    value.
    """
    try:
        f = NoSslFinder()
        ssl = sys.modules.pop("ssl", None)
        sys.meta_path.insert(0, f)
        yield
    finally:
        sys.meta_path.remove(f)
        if ssl:
            sys.modules["ssl"] = ssl


@register_exec_patch("aiohttp.connector")
@contextmanager
def aiohttp_connector_context(module):
    with no_ssl():
        yield


@register_exec_patch("requests.adapters")
@contextmanager
def requests_adapters_context(module):
    with no_ssl():
        yield


@register_exec_patch("urllib3.util.ssl_")
@contextmanager
def urllib3_util_ssl__context(module):
    with no_ssl():
        yield


@register_exec_patch("langsmith._internal._constants")
@contextmanager
def langsmith__internal__constants_context(module):
    # Langsmith uses a UUID to communicate with a background thread. This obviously won't work so we
    # might as well allow it to make a UUID.
    with allow_bad_entropy_calls(1):
        yield


@register_exec_patch("langchain_openai.chat_models.base")
@contextmanager
def langchain_openai_chat_models_base_context(module):
    if sys.version_info >= (3, 13):
        # Creates an ssl context in the version used with 3.13
        with allow_bad_entropy_calls(1):
            yield
    else:
        yield
