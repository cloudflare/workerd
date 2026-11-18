from js import process

# Regression test for Python Workers combined with Node.js compat.
#
# Emscripten's environment detection in `pyodide.asm.mjs` keys off
# `process.versions.node`, so it concludes it is running under Node.js and
# eagerly evaluates `require("node:fs")` while instantiating the Emscripten
# module, which fails with `Dynamic require of "fs" is not supported`.
#
# Loading this worker at all is the actual assertion: if Emscripten takes the
# Node.js branch, the isolate never finishes starting up.

# Sanity check that Node.js compat is really on, so this test can't silently
# stop reproducing the original failure.
assert process.versions.node is not None


def test():
    assert process.versions.node is not None
