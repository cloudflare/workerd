"""
Define import patches that are needed to fix top level entropy calls in the standard library:

1. random: Give it poison seed. Install overlays to crash if called. Check that the seed state has
   not advanced by asserting that the value gotten out of the rng after the snapshot restored
   matches the value that would be generated at poison seed state. Then remove overlays.

2. tempfile: Use a deterministic sequence of names when used at top level.

3. multiprocessing: Just give it poision entropy, the value is ordinarily used for IPC which can't
   happen because we have no processes.

Package import context is defined in entropy_import_context_packages.py
"""

from contextlib import contextmanager
from functools import wraps

try:
    from . import (
        entropy_import_context_packages,  # noqa: F401 Imported for side effects
    )
except ImportError:
    # When the python_process_pth_files compatibility flag is enabled, package entropy import
    # context is provided by workers-runtime-sdk via .pth files instead of being bundled here.
    pass
from .allow_entropy import (
    allow_bad_entropy_calls,
    get_bad_entropy_flag,
    raise_unless_entropy_allowed,
)
from .import_patch_manager import (
    block_calls,
    register_after_snapshot,
    register_before_first_request,
    register_exec_patch,
)

# Exported for snapshot backwards compatibility
__all__ = ["get_bad_entropy_flag"]

# Builtin modules:
# random, tempfile, and multiprocessing

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
