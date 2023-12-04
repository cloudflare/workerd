from pathlib import Path

PRELUDE = """
import {WebAssembly, Date} from "pyodide-internal:builtin_wrappers";

function addEventListener(){}
"""

REPLACEMENTS = [
    ["var _createPyodideModule", "export const _createPyodideModule"],
    ["globalThis._createPyodideModule = _createPyodideModule;", ""],
]


def patch_pyodide(input, output):
    text = Path(input).read_text()

    for [old, new] in REPLACEMENTS:
        text = text.replace(old, new)

    text = PRELUDE + text
    Path(output).write_text(text)
    return 0


def main(argv):
    return patch_pyodide(argv[1], argv[2])


if __name__ == "__main__":
    import sys

    sys.exit(main(sys.argv))
