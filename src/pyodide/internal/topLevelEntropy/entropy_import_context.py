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

from contextlib import contextmanager
from array import array
from .import_patch_manager import block_calls

import sys

RUST_PACKAGES = ["pydantic_core", "tiktoken"]
MODULES_TO_PATCH = ["random", "numpy.random", "numpy.random.mtrand", "tempfile"] + RUST_PACKAGES

# Control number of allowed entropy calls.

ALLOWED_ENTROPY_CALLS = array("b", [0])


def get_bad_entropy_flag():
    # simpleRunPython reads out stderr. We put the address there so we can fish it out...
    # We could use ctypes instead of array but ctypes weighs an extra 100kb compared to array.
    print(ALLOWED_ENTROPY_CALLS.buffer_info()[0], file=sys.stderr)


def is_bad_entropy_enabled():
    """This is used in entropy_patches.py to let calls to disabled functions
    through if we are allowing bad entropy
    """
    return ALLOWED_ENTROPY_CALLS[0] > 0


@contextmanager
def allow_bad_entropy_calls(n):
    ALLOWED_ENTROPY_CALLS[0] = n
    yield
    if ALLOWED_ENTROPY_CALLS[0] > 0:
        raise RuntimeError(
            f"{ALLOWED_ENTROPY_CALLS[0]} unexpected leftover getentropy calls "
        )


# Module instantiation context managers


def get_entropy_import_context(name):
    """Look up the import context.

    If there is a function called <pkg_name>_context, we'll use that. Otherwise,
    we have a default for rust packages. (Currently only used for tiktoken).
    """
    if name not in MODULES_TO_PATCH:
        return None
    funcname = name.replace(".", "_").replace("-", "_") + "_context"
    res = globals().get(funcname, None)
    if res:
        return res
    if name in RUST_PACKAGES:
        # Initial import needs one entropy call to initialize std::collections::HashMap hash seed
        return rust_package_context
    raise Exception(f"Missing context for {name}")


@contextmanager
def rust_package_context(module):
    """Rust packages need one entropy call if they create a rust hash map at
    init time."""
    with allow_bad_entropy_calls(1):
        yield


@contextmanager
def random_context(module):
    """Importing random calls getentropy() 10 times it seems"""
    with allow_bad_entropy_calls(10):
        yield
    # Block calls to functions that use the bad random seed we produced from the
    # ten getentropy() calls. Instantiating Random with a given seed is fine,
    # instantiating it without a seed will call getentropy() and fail.
    # Instantiating SystemRandom is fine, calling it's methods will call
    # getentropy() and fail.
    block_calls(module, allowlist=["Random", "SystemRandom"])


@contextmanager
def numpy_random_context(module):
    """numpy.random doesn't call getentropy() itself, but we want to block calls
    that might use the bad seed.

    TODO: Maybe there are more calls we can whitelist?
    TODO: Is it not enough to just block numpy.random.mtrand calls?
    """
    yield
    # Calling default_rng() with a given seed is fine, calling it without a seed
    # will call getentropy() and fail.
    block_calls(module, allowlist=["default_rng"])


@contextmanager
def numpy_random_mtrand_context(module):
    # numpy.random.mtrand calls secrets.randbits at top level to seed itself.
    # This will fail if we don't let it through.
    with allow_bad_entropy_calls(1):
        yield
    # Block calls until we get a chance to replace the bad random seed.
    block_calls(module)


@contextmanager
def pydantic_core_context(module):
    try:
        # Initial import needs one entropy call to initialize std::collections::HashMap hash seed
        with allow_bad_entropy_calls(1):
            yield
    finally:
        try:
            with allow_bad_entropy_calls(1):
                # validate_core_schema makes an ahash::AHashMap which makes another entropy call for
                # its hash seed. It will throw an error but only after making the needed entropy
                # call.
                module.validate_core_schema(None)
        except module.SchemaError:
            pass


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


@contextmanager
def tempfile_context(module):
    yield
    module._orig_RandomNameSequence = module._RandomNameSequence
    module._RandomNameSequence = DeterministicRandomNameSequence


def tempfile_restore_random_name_sequence():
    import tempfile

    tempfile._RandomNameSequence = tempfile._orig_RandomNameSequence
    del tempfile._orig_RandomNameSequence
