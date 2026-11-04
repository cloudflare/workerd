from importlib.machinery import PathFinder

import numpy as np
from workers import WorkerEntrypoint


class Default(WorkerEntrypoint):
    async def test(self):
        res = np.arange(12).reshape((3, -1))[::-2, ::-2]
        assert str(res) == "[[11  9]\n [ 3  1]]"

        # numpy was imported at the top level, which doesn't run again when a dedicated snapshot
        # is restored. Search sys.path directly to check that it still resolves the vendored
        # package.
        spec = PathFinder.find_spec("numpy")
        assert spec is not None
        assert spec.origin.startswith("/session/metadata/python_modules/"), spec.origin
