# The stdlib zip is served read-only from the Pyodide bundle, which every isolate shares.
import pathlib
import sys
import zipfile


def test():
    zip_path = next(p for p in sys.path if p.endswith(".zip"))
    assert pathlib.Path(zip_path).is_symlink(), zip_path
    assert pathlib.Path(zip_path).is_file(), zip_path

    with zipfile.ZipFile(zip_path) as zf:
        assert "os.py" in zf.namelist()
        assert zf.testzip() is None

    # The file can't be written, and a failed write leaves it intact.
    try:
        with open(zip_path, "r+b") as f:
            f.write(b"corrupted")
    except OSError:
        pass
    else:
        raise AssertionError("writing to the stdlib zip should fail")

    with open(zip_path, "rb") as f:
        assert f.read(4) == b"PK\x03\x04"
