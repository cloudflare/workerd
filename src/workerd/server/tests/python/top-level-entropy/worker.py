import os
import secrets
from random import getrandbits, random

from _cloudflare.allow_entropy import (
    allow_bad_entropy,
    allow_bad_entropy_calls,
)
from workers import WorkerEntrypoint


def assert_top_level_entropy_denied(call):
    try:
        call()
    except OSError as error:
        message = str(error)
        assert "PYTHON_WORKERS_ALLOW_TOP_LEVEL_ENTROPY" in message
        assert "https://github.com/cloudflare/workers-py/issues/new" in message
    else:
        import os

        print(os.environ)
        from _cloudflare.allow_entropy import TOP_LEVEL_ENTROPY_CONFIGURATION

        print(TOP_LEVEL_ENTROPY_CONFIGURATION)
        raise AssertionError("top-level entropy call unexpectedly succeeded")


def assert_blocked_random(call):
    try:
        call()
    except RuntimeError:
        pass
    else:
        raise AssertionError("random call unexpectedly succeeded")


# 1. Without environment variable set, top-level entropy should be denied
assert_top_level_entropy_denied(lambda: os.urandom(1))

# 2. Values other than "1" do not enable top-level entropy
os.environ["PYTHON_WORKERS_ALLOW_TOP_LEVEL_ENTROPY"] = "allowed_module"
assert_top_level_entropy_denied(lambda: os.urandom(1))
assert_blocked_random(lambda: getrandbits(64))

# 3. A finite entropy budget applies to random and os.urandom calls
with allow_bad_entropy_calls(3):
    assert isinstance(getrandbits(64), int)
    assert len(os.urandom(1)) == 1
    assert len(os.urandom(1)) == 1
    # After 3 calls, the budget should be exhausted
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

# 6. With bad entropy allowed, we can use all the entropy functions
with allow_bad_entropy():
    for _ in range(128):
        assert len(os.urandom(1)) == 1
    assert isinstance(getrandbits(64), int)
    assert isinstance(random(), float)

# 7. Outside of a request context, top-level entropy should still be denied
assert_blocked_random(lambda: getrandbits(64))
assert_top_level_entropy_denied(lambda: secrets.token_bytes(1))


class Default(WorkerEntrypoint):
    async def test(self):
        # These should all work now that we're in a request context
        assert isinstance(getrandbits(64), int)
        assert 0 <= random() < 1
        assert len(secrets.token_bytes(32)) == 32
        assert len(os.urandom(32)) == 32
        assert os.urandom(32) != os.urandom(32)
