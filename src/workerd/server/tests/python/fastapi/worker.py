"""
This just tests that we can import all the dependencies.
We don't make any requests here, so it doesn't check that
part.

TODO: update this to test that something happened
"""

import numpy.random.mtrand

def test():
    from fastapi import FastAPI, Request

    raise RuntimeError("test")

