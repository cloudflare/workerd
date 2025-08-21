# Control number of allowed entropy calls.
import sys
from array import array
from contextlib import contextmanager

ALLOWED_ENTROPY_CALLS = array("b", [0])
IN_REQUEST_CONTEXT = False


def in_request_context():
    return IN_REQUEST_CONTEXT


def _set_in_request_context():
    global IN_REQUEST_CONTEXT

    IN_REQUEST_CONTEXT = True


def should_allow_entropy_call():
    """This helps us raise Python errors rather than fatal errors in some cases.

    It doesn't really matter that much since we're not likely to recover from
    these anyways but it feels better.
    """
    # Allow if we've either entered request context or if we've temporarily
    # enabled entropy.
    return IN_REQUEST_CONTEXT or is_bad_entropy_enabled()


def raise_unless_entropy_allowed():
    if not should_allow_entropy_call():
        EIO = 29
        raise OSError(EIO, "Cannot get entropy outside of request context")


def get_bad_entropy_flag():
    # simpleRunPython reads out stderr. We put the address there so we can fish
    # it out... We could use ctypes instead of array but ctypes weighs an extra
    # 100kb compared to array.
    print(ALLOWED_ENTROPY_CALLS.buffer_info()[0], file=sys.stderr)


def is_bad_entropy_enabled():
    """This is used in entropy_patches.py to let calls to disabled functions
    through if we are allowing bad entropy
    """
    return ALLOWED_ENTROPY_CALLS[0] > 0


@contextmanager
def allow_bad_entropy_calls(n):
    old_allowed_entropy_calls = ALLOWED_ENTROPY_CALLS[0]
    ALLOWED_ENTROPY_CALLS[0] = n
    yield
    if ALLOWED_ENTROPY_CALLS[0] > 0:
        raise RuntimeError(
            f"{ALLOWED_ENTROPY_CALLS[0]} unexpected leftover getentropy calls "
        )
    ALLOWED_ENTROPY_CALLS[0] = old_allowed_entropy_calls
