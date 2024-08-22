"""
Handle the top level getentropy() mess:

The C stdlib function getentropy() `getentropy()` calls
`crpyto.getRandomValues()` but this throws an error at top level which causes a
fatal error.

Goals:

1. Avoid top-level calls to the C stdlib function getentropy(), these fatally
   fail. Patch these to raise Python errors instead.
2. Allow top level import of `random` and `numpy.random` modules. These seed
   themselves with the functions that we patched in step 1, we temporarily
   replace the `getentropy()` calls with no-ops to let them through.
3. Install wrapper modules at top level that only allow calls to a whitelisted
   set of functions from `random` and `numpy.random` that don't use the bad
   seeds that came from step 2.
4. Put it all back.
5. Reseed the rng before entering the request scope for the first time.

Steps 1, part of 4, and 5 are handled here, steps 2, 3, and part of 4 are
handled in _cloudflare_random_overlays.
"""

import _random
import os
import sys
from functools import wraps

from .entropy_import_context import (
    get_entropy_import_context,
    is_bad_entropy_enabled,
    tempfile_restore_random_name_sequence,
)
from .import_patch_manager import (
    install_import_patch_manager,
    remove_import_patch_manager,
)

IN_REQUEST_CONTEXT = False


def should_allow_entropy_call():
    """This helps us raise Python errors rather than fatal errors in some cases.

    It doesn't really matter that much since we're not likely to recover from
    these anyways but it feels better.
    """
    # Allow if we've either entered request context or if we've temporarily
    # enabled entropy.
    return IN_REQUEST_CONTEXT or is_bad_entropy_enabled()


# Step 1.
#
# Prevent calls to getentropy(). The intended way for `getentropy()` to fail is
# to set an EIO error, which turns into a Python OSError, so we raise this same
# error so that if we patch `getentropy` from the Emscripten C stdlib we can
# remove these patches without changing the behavior.

EIO = 29

orig_urandom = os.urandom


@wraps(orig_urandom)
def patch_urandom(*args):
    if not should_allow_entropy_call():
        raise OSError(EIO, "Cannot get entropy outside of request context")
    return orig_urandom(*args)


def disable_urandom():
    """
    Python os.urandom() calls C getentropy() which calls JS
    crypto.getRandomValues() which throws at top level, fatally crashing the
    interpreter.

    TODO: Patch Emscripten's getentropy() to return EIO if
    `crypto.getRandomValues()` throws. Then we can remove this.
    """
    os.urandom = patch_urandom


def restore_urandom():
    os.urandom = orig_urandom


orig_Random_seed = _random.Random.seed


@wraps(orig_Random_seed)
def patched_seed(self, val):
    """
    Random.seed calls _PyOs_URandom which will fatally fail in top level.
    Prevent this by raising a RuntimeError instead.
    """
    if val is None and not should_allow_entropy_call():
        raise OSError(EIO, "Cannot get entropy outside of request context")
    return orig_Random_seed(self, val)


def disable_random_seed():
    # Install patch to block calls to PyOs_URandom
    _random.Random.seed = patched_seed


def restore_random_seed():
    # Restore original random seed behavior
    _random.Random.seed = orig_Random_seed


def reseed_rng():
    """
    Step 5: Have to reseed randomness in the IoContext of the first request
    since we gave a low quality seed when it was seeded at top level.
    """
    from random import seed

    seed()

    if "numpy.random" in sys.modules:
        from numpy.random import seed

        seed()


def before_top_level():
    disable_urandom()
    disable_random_seed()
    install_import_patch_manager(get_entropy_import_context)


def before_first_request():
    global IN_REQUEST_CONTEXT

    IN_REQUEST_CONTEXT = True
    restore_urandom()
    restore_random_seed()
    remove_import_patch_manager()
    reseed_rng()
    tempfile_restore_random_name_sequence()
