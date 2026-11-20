# The bundle's stdlib zip has a precompiled .pyc next to every .py (see
# src/pyodide/bundle/compile_stdlib.py), so importing a stdlib module doesn't compile its source.
import sys
import zipfile


def test():
    zip_path = next(p for p in sys.path if p.endswith(".zip"))
    with zipfile.ZipFile(zip_path) as zf:
        names = set(zf.namelist())
    sources = [n for n in names if n.endswith(".py")]
    assert sources
    missing = [n for n in sources if n[:-3] + ".pyc" not in names]
    assert not missing, missing[:10]

    # Nothing imports colorsys at startup, so this import reads it from the zip now.
    assert "colorsys" not in sys.modules
    import colorsys

    assert colorsys.__file__ == f"{zip_path}/colorsys.pyc", colorsys.__file__
    # The bytecode names the source, for tracebacks.
    filename = colorsys.rgb_to_hsv.__code__.co_filename
    assert filename == f"{zip_path}/colorsys.py", filename
