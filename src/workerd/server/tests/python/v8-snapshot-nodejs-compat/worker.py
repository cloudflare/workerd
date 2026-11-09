# Runs in a worker with nodejs_compat, started from a V8 heap snapshot that the worker without
# it (plain_worker.py) built. The behaviour must be the same as loading Pyodide from scratch.
import asyncio
import sys

import js
from js import globalThis

from pyodide.ffi import run_sync
from workers import WorkerEntrypoint

# State created at the top level of the worker, i.e. after the snapshot.
TOP_LEVEL = {"n": sum(range(1000))}


def test():
    import pyodide_js

    assert pyodide_js._api.restoredFromV8Snapshot is True

    # Python's `js` module is this worker's global scope, not the snapshot's.
    assert js == globalThis
    assert hasattr(js, "Response")
    assert hasattr(js, "fetch")
    assert TOP_LEVEL["n"] == 499500

    # The snapshot was taken without any knowledge of compatibility flags; what Python sees
    # through `js` is this worker's scope, with nodejs_compat's additions.
    # (Pyodide cannot proxy Node Buffer *instances*; the constructor is visible, which is what
    # tells the two worker configurations apart.)
    assert hasattr(js, "Buffer")
    assert js.Buffer.name == "Buffer"
    assert hasattr(js, "process")
    assert js.process.env is not None
    assert not hasattr(globalThis, "definitely_not_a_global")

    # The baseline imports are present and functional.
    for name in ("asyncio", "zipfile", "typing", "inspect", "re"):
        assert name in sys.modules, name
    import re

    assert re.sub("a+", "b", "caaat") == "cbt"

    # Host integration points that the snapshot cannot carry: timers and randomness.
    import random

    assert 0 <= random.random() < 1

    # The restored context is as restricted as the worker's: Python's clock is the worker's
    # performance.now() (Emscripten reads it from the restored context's global, which forwards
    # to the worker's), and it cannot compile code from strings.
    import time

    from pyodide.ffi import create_proxy

    real_now = js.performance.now
    fake_now = create_proxy(lambda *_: 1234567.0)
    js.performance.now = fake_now
    try:
        assert time.perf_counter() == 1234.567, time.perf_counter()
    finally:
        js.performance.now = real_now
        fake_now.destroy()
    try:
        pyodide_js.constructor.constructor("return 1")()
    except Exception as e:  # noqa: BLE001
        assert "EvalError" in type(e).__name__ or "disallowed" in str(e).lower(), repr(e)
    else:
        raise AssertionError("eval was allowed in the restored context")

    async def later():
        await asyncio.sleep(0.01)
        return 7

    assert run_sync(later()) == 7
    assert run_sync(js.Promise.resolve(41)) + 1 == 42

    # Calling back from JS into Python.
    assert js.Array.of(0, 1, 2).map(lambda x, *_: x * 2).to_py() == [0, 2, 4]


class Default(WorkerEntrypoint):
    async def test(self):
        test()
