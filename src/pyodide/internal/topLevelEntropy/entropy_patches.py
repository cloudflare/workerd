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

import os
from functools import wraps

# Import entropy_import_context for side effects
from . import entropy_import_context  # noqa: F401
from .allow_entropy import _set_in_request_context, raise_unless_entropy_allowed
from .import_patch_manager import (
    before_first_request_handlers,
    install_import_patch_manager,
    remove_import_patch_manager,
)

# Prevent calls to getentropy(). The intended way for `getentropy()` to fail is
# to set an EIO error, which turns into a Python OSError, so we raise this same
# error so that if we patch `getentropy` from the Emscripten C stdlib we can
# remove these patches without changing the behavior.


orig_urandom = os.urandom


@wraps(orig_urandom)
def patch_urandom(*args):
    raise_unless_entropy_allowed()
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


def before_top_level():
    disable_urandom()
    install_import_patch_manager()


def before_first_request():
    _set_in_request_context()
    restore_urandom()
    remove_import_patch_manager()
    for cb in before_first_request_handlers:
        cb()
