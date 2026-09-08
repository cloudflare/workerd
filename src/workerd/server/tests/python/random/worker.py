"""
Verify that calling `random` at the top-level throws.

Calls to random should only work inside a request context.
"""

# Disable do not `assert False` lint
# ruff: noqa: B011

from random import choice, randbytes, random


def assert_blocked(call):
    try:
        call()
    except RuntimeError:
        pass
    else:
        assert False


assert_blocked(random)
assert_blocked(lambda: randbytes(5))
assert_blocked(lambda: choice([1, 2, 3]))


def t1():
    from random import randbytes, random

    return random(), randbytes(5), choice([1, 2, 3])


def t2():
    first = random(), randbytes(5), choice([1, 2, 3])
    second = t1()
    return first, second


def test():
    for value, data, selection in t2():
        assert 0 <= value < 1
        assert len(data) == 5
        assert selection in [1, 2, 3]
