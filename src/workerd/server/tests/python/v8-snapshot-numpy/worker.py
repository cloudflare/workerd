# Loads a vendored package with native extensions in a worker started from a V8 heap snapshot of
# Pyodide. The extensions' shared libraries are compiled by the worker's context and instantiated
# by the snapshot's, and they live on a filesystem mounted after the snapshot was taken; numpy
# itself imports many of them.
import pyodide_js
from workers import WorkerEntrypoint


class Default(WorkerEntrypoint):
    async def test(self):
        assert pyodide_js._api.restoredFromV8Snapshot is True
        import numpy as np

        res = np.arange(12).reshape((3, -1))[::-2, ::-2]
        assert str(res) == "[[11  9]\n [ 3  1]]"
        assert np.linalg.det(np.eye(3)) == 1.0
