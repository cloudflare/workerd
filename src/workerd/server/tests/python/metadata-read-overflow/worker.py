# Ensure that manipulating the file in the readonly filesystem doesn't cause overflow
import pyodide_js


def test():
    mod = pyodide_js._module
    FS = mod.FS
    path = "/session/metadata/payload.dat"
    lookup = FS.lookupPath(path)
    node = lookup.node

    old_used = node.usedBytes
    old_index = node.index

    try:
        for size in (0x7FFFFFFF, 0x80000000, 0x80000001):
            node.usedBytes = size
            node.index = old_index
            try:
                mod.compileModuleFromReadOnlyFS(mod, path)
            except Exception:
                pass
    finally:
        node.usedBytes = old_used
        node.index = old_index
