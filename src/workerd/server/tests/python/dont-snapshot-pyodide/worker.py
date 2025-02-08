"""
To trigger the bug we need to do two things:
1. import `pyodide` at top level
2. ensure that there is some package requirement in wd-test
3. make test() async

Importing numpy isn't really necessary but we need to include it as a requirement in the wd-test
file so that we consider making a package snapshot. In the buggy code, importing pyodide at top
level then makes the package snapshot import pyodide while making the snapshot. Importing pyodide
before calling finalizeBootstrap messes up the runtime state and causes various weird and malign
symptoms.
"""

import numpy

import pyodide


async def test():
    # Mention imports so that ruff won't remove them
    pyodide  # noqa: B018
    numpy  # noqa: B018
