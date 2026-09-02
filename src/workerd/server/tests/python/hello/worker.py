import js
from js import console, globalThis

from pyodide import __version__
from pyodide.ffi import JsException

if __version__ == "0.26.0a2":
    assert js == globalThis
else:
    # js should be patched at top level in order to allow snapshotting.
    assert js != globalThis


# Calling fetch at top level should give a good error message despite patching. Depending on the
# compatibility date, fetch either throws synchronously or returns a rejected promise.
def check_fetch_error(error):
    print(error)
    assert "Disallowed operation called within global scope" in str(error)


try:
    fetch_result = js.fetch("example.com")
except JsException as e:
    check_fetch_error(e)
else:
    fetch_result.catch(check_fetch_error)
    del fetch_result


def test():
    # js should be correctly restored when restoring snapshot
    assert js == globalThis

    # This just tests that nothing raises when we run this. It isn't great though
    # because we don't test whether we printed anything.
    # TODO: update this to test that something happened
    print("Does this work?")
    console.log("Does this work?")  # both should print the same output
