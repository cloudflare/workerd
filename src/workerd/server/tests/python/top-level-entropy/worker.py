import os
from random import choice, randbytes, random

from _cloudflare.allow_entropy import (
    allow_bad_entropy_calls,
)
from workers import WorkerEntrypoint


def assert_top_level_entropy_denied(call):
    try:
        call()
    except OSError as e:
        assert "Randomness is not allowed" in str(e), f"Unexpected error message: {e!s}"
    else:
        raise AssertionError("top-level entropy call unexpectedly succeeded")


def assert_top_level_entropy_denied_random(call):
    try:
        call()
    except RuntimeError as e:
        assert "outside of request context" in str(e), (
            f"Unexpected error message: {e!s}"
        )
        assert "Randomness is not allowed" in str(e), f"Unexpected error message: {e!s}"
    else:
        raise AssertionError("top-level entropy call unexpectedly succeeded")


# 1. Without environment variable set, top-level entropy should be denied
assert_top_level_entropy_denied(lambda: os.urandom(1))

# 3. A finite entropy budget applies to random and os.urandom calls
with allow_bad_entropy_calls(2):
    assert len(os.urandom(1)) == 1
    assert len(os.urandom(1)) == 1
    # After 2 calls, the budget should be exhausted
    assert_top_level_entropy_denied(lambda: os.urandom(1))

# 4. After the context manager exits, the budget should be reset
assert_top_level_entropy_denied(lambda: os.urandom(1))

# 5. When the context manager exits without exhausting the budget, it should raise an exception
try:
    with allow_bad_entropy_calls(1):
        pass
except RuntimeError:
    pass
else:
    raise AssertionError("leftover entropy budget unexpectedly succeeded")

# 6. Calling random functions should fail as well, but with different error type
assert_top_level_entropy_denied_random(lambda: random())
assert_top_level_entropy_denied_random(lambda: randbytes(1))
assert_top_level_entropy_denied_random(lambda: choice([1, 2, 3]))


class Default(WorkerEntrypoint):
    async def test(self):
        # These should all work now that we're in a request context
        assert len(os.urandom(32)) == 32
        assert os.urandom(32) != os.urandom(32)
        assert 0.0 <= random() <= 1.0
        assert len(randbytes(16)) == 16
        assert choice([1, 2, 3]) in [1, 2, 3]
