#!/usr/bin/env python3
"""Build-time tool: add a precompiled .pyc next to every .py in Pyodide's stdlib zip.

Pyodide's python_stdlib.zip ships source only, so every stdlib module the baseline snapshot hasn't
already imported is compiled from source, in Wasm, on each cold start. zipimport tries
`module.pyc` before `module.py`, so with the bytecode in the zip it only unmarshals it.

The .pyc files are unchecked hash-based (PEP 552): zipimport uses them without reading the
source. That is safe because both come from this zip and are never edited apart. The .py files
stay, for tracebacks. The bytecode format changes between minor versions, so this must run on
the bundle's own Python minor version, which `--python-version` asserts.
"""

import argparse
import importlib.util
import sys
import zipfile
from importlib._bootstrap_external import _code_to_hash_pyc


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, help="Pyodide's python_stdlib.zip")
    parser.add_argument("--output", required=True)
    parser.add_argument(
        "--python-version",
        required=True,
        help="the bundle's Python version, e.g. 3.13; must match this interpreter's",
    )
    args = parser.parse_args()

    running = f"{sys.version_info.major}.{sys.version_info.minor}"
    if args.python_version.split(".")[:2] != running.split("."):
        sys.exit(
            f"compile_stdlib: the bundle's Python is {args.python_version}, "
            f"but this runs on {running}"
        )
    # Where Pyodide mounts the zip, so that tracebacks name the source in it.
    archive = f"/lib/python{sys.version_info.major}{sys.version_info.minor}.zip"

    with (
        zipfile.ZipFile(args.input) as src,
        zipfile.ZipFile(args.output, "w", zipfile.ZIP_DEFLATED) as dst,
    ):
        names = set(src.namelist())
        for info in src.infolist():
            data = src.read(info)
            dst.writestr(info, data)
            if not info.filename.endswith(".py"):
                continue
            pyc_name = info.filename[:-3] + ".pyc"
            if pyc_name in names:
                continue
            code = compile(
                data, f"{archive}/{info.filename}", "exec", dont_inherit=True
            )
            pyc = _code_to_hash_pyc(
                code, importlib.util.source_hash(data), checked=False
            )
            # The entry keeps its source's timestamp, so the output only depends on the input.
            pyc_info = zipfile.ZipInfo(pyc_name, date_time=info.date_time)
            pyc_info.compress_type = zipfile.ZIP_DEFLATED
            pyc_info.external_attr = info.external_attr
            dst.writestr(pyc_info, bytes(pyc))


if __name__ == "__main__":
    main()
