"""
Manage import context for modules that use getentropy() at startup.

We install a metapath finder in import_patch_manager.py which executes the
module in the context manager returned by
get_entropy_import_context(module_name).

This module defines get_entropy_import_context which

"random" and "numpy.random.mtrand" also have some additional patches that need
to be installed as part of their import context to prevent top level crashes.

Other rust packages are likely to need similar treatment to pydantic_core.
"""

import sys
from contextlib import contextmanager
from functools import wraps

from .allow_entropy import (
    allow_bad_entropy_calls,
    get_bad_entropy_flag,
    raise_unless_entropy_allowed,
)
from .import_patch_manager import (
    block_calls,
    register_after_snapshot,
    register_before_first_request,
    register_create_patch,
    register_exec_patch,
)

# Exported for snapshot backwards compatibility
__all__ = ["get_bad_entropy_flag"]

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


RANDOM_STATE = None


@register_exec_patch("random")
@contextmanager
def random_exec(random):
    """Importing random calls getentropy() 10 times it seems"""
    with allow_bad_entropy_calls(10):
        yield

    # Block calls to functions that use the bad random seed we produced from the
    # ten getentropy() calls. Instantiating Random with a given seed is fine,
    # instantiating it without a seed will call getentropy() and fail.
    # Instantiating SystemRandom is fine, calling its methods will call
    # getentropy() and fail.
    global RANDOM_STATE
    RANDOM_STATE = random.getstate()
    block_calls(random, allowlist=("Random", "SystemRandom"))


@register_after_snapshot("random")
def random_after_snapshot(random):
    # Check that random seed hasn't been advanced somehow while executing top level scope
    r1 = random.random()
    random.setstate(RANDOM_STATE)
    r2 = random.random()
    if r1 != r2:
        raise RuntimeError("random seed in bad state")


@register_before_first_request("random")
def random_before_first_request(random):
    random.seed()


orig_Random_seed = None


@register_exec_patch("_random")
@contextmanager
def _random_exec(_random):
    yield
    global orig_Random_seed
    orig_Random_seed = _random.Random.seed

    @wraps(orig_Random_seed)
    def patched_seed(self, val):
        """
        Random.seed calls _PyOs_URandom which will fatally fail in top level.
        Prevent this by raising a RuntimeError instead.
        """
        if val is None:
            raise_unless_entropy_allowed()
        return orig_Random_seed(self, val)

    _random.Random.seed = patched_seed


@register_before_first_request("_random")
def _random_before_first_request(_random):
    _random.Random.seed = orig_Random_seed


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


class DeterministicRandomNameSequence:
    characters = "abcdefghijklmnopqrstuvwxyz0123456789_"

    def __init__(self):
        self.index = 0

    def __iter__(self):
        return self

    def index_to_chars(self):
        base = len(self.characters)
        idx = self.index
        s = []
        for _ in range(8):
            s.append(self.characters[idx % base])
            idx //= base
        return "".join(s)

    def __next__(self):
        self.index += 1
        return self.index_to_chars()


@register_exec_patch("tempfile")
@contextmanager
def tempfile_context(module):
    yield
    module._orig_RandomNameSequence = module._RandomNameSequence
    module._RandomNameSequence = DeterministicRandomNameSequence


@register_before_first_request("tempfile")
def tempfile_restore_random_name_sequence(tempfile):
    tempfile._RandomNameSequence = tempfile._orig_RandomNameSequence
    del tempfile._orig_RandomNameSequence


@register_exec_patch("multiprocessing.process")
@contextmanager
def multiprocessing_process_context(module):
    # multiprocessing.process calls os.urandom() on import. It's harmless b/c multiprocessing is
    # useless.
    with allow_bad_entropy_calls(1):
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
