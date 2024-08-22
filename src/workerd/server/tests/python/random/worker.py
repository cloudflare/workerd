"""
Verify that calling `random` at the top-level throws.

Calls to random should only work inside a request context.
"""

# Disable do not `assert False` lint
# ruff: noqa: B011

from random import choice, randbytes, random

try:
    random()
except RuntimeError as e:
    assert (
        repr(e)
        == "RuntimeError('Cannot use random.random() outside of request context')"
    )
else:
    assert False

try:
    randbytes(5)
except RuntimeError as e:
    assert (
        repr(e)
        == "RuntimeError('Cannot use random.randbytes() outside of request context')"
    )
else:
    assert False

try:
    choice([1, 2, 3])
except RuntimeError as e:
    assert (
        repr(e)
        == "RuntimeError('Cannot use random.choice() outside of request context')"
    )
else:
    assert False


def t1():
    from random import randbytes, random

    random()
    randbytes(5)
    choice([1, 2, 3])


def t2():
    random()
    randbytes(5)
    choice([1, 2, 3])

    t1()


def test():
    t2()
