# Control number of allowed entropy calls.
import os
import sys
from array import array
from contextlib import contextmanager

ALLOWED_ENTROPY_CALLS = array("b", [0])
IN_REQUEST_CONTEXT = False
# TODO(maybe): for now, this is a boolean flag, but we could extend it to support
# specific modules if needed
PYTHON_WORKERS_ALLOW_TOP_LEVEL_ENTROPY = "PYTHON_WORKERS_ALLOW_TOP_LEVEL_ENTROPY"
TOP_LEVEL_ENTROPY_CONFIGURATION = False

TOP_LEVEL_ENTROPY_ERROR = (
    "Randomness is not allowed while a Worker is starting because startup "
    "values will be repeated across Worker instances. If this error occurs from "
    "importing a package, import the package from a function to load it after the Worker starts. "
    "If it must load at startup, set "
    'os.environ["PYTHON_WORKERS_ALLOW_TOP_LEVEL_ENTROPY"] = "1" '
    "before importing it to allow randomness for all startup code. "
    "Do not use this for secrets or unique IDs. Please report the package at "
    "https://github.com/cloudflare/workers-py/issues/new."
)


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
        raise OSError(EIO, TOP_LEVEL_ENTROPY_ERROR)


def get_bad_entropy_flag():
    # simpleRunPython reads out stderr. We put the address there so we can fish
    # it out... We could use ctypes instead of array but ctypes weighs an extra
    # 100kb compared to array.
    print(ALLOWED_ENTROPY_CALLS.buffer_info()[0], file=sys.stderr)


def is_bad_entropy_enabled():
    """This is used in entropy_patches.py to let calls to disabled functions
    through if we are allowing bad entropy
    """
    return is_top_level_entropy_enabled() or ALLOWED_ENTROPY_CALLS[0] != 0


def consume_bad_entropy_call():
    """
    Similar to shouldAllowBadEntropy in JS but for python random module.
    Python's random module do not use crypto.getRandomValues directly, so we need to track
    the calls here.
    """
    allow_all = is_top_level_entropy_enabled()
    value = ALLOWED_ENTROPY_CALLS[0]
    if allow_all or value == -1:
        return True
    if value > 0:
        ALLOWED_ENTROPY_CALLS[0] -= 1
        return True
    if value == 0:
        return False
    raise RuntimeError(f"Unexpected randomness allowance value: {value}")


def is_top_level_entropy_enabled():
    global TOP_LEVEL_ENTROPY_CONFIGURATION

    if not TOP_LEVEL_ENTROPY_CONFIGURATION:
        TOP_LEVEL_ENTROPY_CONFIGURATION = (
            os.environ.get(PYTHON_WORKERS_ALLOW_TOP_LEVEL_ENTROPY, "").strip() == "1"
        )
    if TOP_LEVEL_ENTROPY_CONFIGURATION:
        ALLOWED_ENTROPY_CALLS[0] = -1
    return TOP_LEVEL_ENTROPY_CONFIGURATION


def clear_global_entropy():
    global TOP_LEVEL_ENTROPY_CONFIGURATION

    TOP_LEVEL_ENTROPY_CONFIGURATION = False
    ALLOWED_ENTROPY_CALLS[0] = 0


@contextmanager
def allow_bad_entropy_calls(n):
    old_allowed_entropy_calls = ALLOWED_ENTROPY_CALLS[0]
    if old_allowed_entropy_calls == -1:
        yield
        return

    ALLOWED_ENTROPY_CALLS[0] = n
    try:
        yield
    finally:
        leftover_entropy_calls = ALLOWED_ENTROPY_CALLS[0]
        allow_all = is_top_level_entropy_enabled()
        ALLOWED_ENTROPY_CALLS[0] = -1 if allow_all else old_allowed_entropy_calls

    if not allow_all and leftover_entropy_calls > 0:
        raise RuntimeError(
            f"{leftover_entropy_calls} unexpected leftover getentropy calls"
        )


@contextmanager
def allow_bad_entropy():
    with allow_bad_entropy_calls(-1):
        yield
