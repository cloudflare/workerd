import importlib
import os
from random import getrandbits, random

from workers import WorkerEntrypoint

# This worker has top-level entropy globally allowed
os.environ["PYTHON_WORKERS_ALLOW_TOP_LEVEL_ENTROPY"] = "1"

assert len(os.urandom(32)) == 32
assert isinstance(getrandbits(64), int)
importlib.import_module("allowed_module_extra")


class Default(WorkerEntrypoint):
    async def test(self):
        assert isinstance(getrandbits(64), int)
        assert 0 <= random() < 1
        assert len(os.urandom(32)) == 32
        assert os.urandom(32) != os.urandom(32)
