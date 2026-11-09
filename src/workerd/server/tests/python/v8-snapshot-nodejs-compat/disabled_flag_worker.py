# A worker that turns off a flag Pyodide reads while loading and that predates its Pyodide
# release: not a configuration workerd builds a V8 heap snapshot for, even under the autogate. It
# loads Pyodide from scratch.
import pyodide_js
from workers import WorkerEntrypoint


def test():
    assert getattr(pyodide_js._api, "restoredFromV8Snapshot", False) is False


class Default(WorkerEntrypoint):
    async def test(self):
        test()
