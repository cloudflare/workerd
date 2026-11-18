import js
from js import console, globalThis

from pyodide import __version__
from pyodide.ffi import JsException

if __version__ == "0.26.0a2":
    assert js == globalThis
else:
    # js should be patched at top level in order to allow snapshotting.
    assert js != globalThis

# Calling fetch at top level should give a good error message despite patching.
try:
    js.fetch("example.com")
except JsException as e:
    print(e)
    assert "Disallowed operation called within global scope" in str(e)
else:
    assert False  # noqa: B011


def test():
    # js should be correctly restored when restoring snapshot
    assert js == globalThis

    # This just tests that nothing raises when we run this. It isn't great though
    # because we don't test whether we printed anything.
    # TODO: update this to test that something happened
    print("Does this work?")
    console.log("Does this work?")  # both should print the same output
