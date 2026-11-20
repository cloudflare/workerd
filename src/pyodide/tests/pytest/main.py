import os

import pytest
from workers import WorkerEntrypoint

from pyodide.webloop import WebLoop


async def noop(*args):
    pass


# pytest-asyncio relies on these but in Pyodide < 0.29 WebLoop does not implement them
WebLoop.shutdown_asyncgens = noop
WebLoop.shutdown_default_executor = noop


class Default(WorkerEntrypoint):
    async def test(self):
        os.chdir("/session/metadata/tests")
        assert pytest.main([".", "-vv"]) == 0
