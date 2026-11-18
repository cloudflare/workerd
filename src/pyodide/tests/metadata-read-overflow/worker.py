# Ensure that a read from the readonly filesystem into a buffer of 2 GiB or more doesn't overflow
# the size arithmetic in PyodideMetadataReader::read().
import pyodide_js
from js import Object, Uint8Array, WebAssembly

SIZES = (0x7FFFFFFF, 0x80000000, 0x80000001)
WASM_PAGE_SIZE = 0x10000


def test():
    mod = pyodide_js._module
    FS = mod.FS
    path = "/session/metadata/payload.dat"
    node = FS.lookupPath(path).node

    with open(path, "rb") as f:
        expected = f.read()

    # The pages of a WebAssembly.Memory become resident only when written, and the reader writes
    # just the file's bytes, so these multi-GiB views cost a few KiB. (A resizable ArrayBuffer
    # wouldn't do: jsg::asBytes() copies a view of one.)
    opts = Object.new()
    opts.initial = (max(SIZES) + WASM_PAGE_SIZE - 1) // WASM_PAGE_SIZE
    backing = WebAssembly.Memory.new(opts).buffer

    old_used = node.usedBytes
    stream = FS.open(path, 0)  # O_RDONLY
    try:
        for size in SIZES:
            # The read is clamped to usedBytes, so it has to be at least the buffer's size for the
            # full-length buffer to reach the reader.
            node.usedBytes = size
            buffer = Uint8Array.new(backing, 0, size)
            n = FS.read(stream, buffer, 0, size, 0)
            assert n == len(expected), (hex(size), n)
            assert bytes(buffer.subarray(0, n).to_py()) == expected, hex(size)
    finally:
        FS.close(stream)
        node.usedBytes = old_used
