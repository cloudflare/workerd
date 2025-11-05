import sys


def test():
    try:
        from a import A  # noqa: F401

        raise Exception("Should not be able to import a")  # noqa: TRY002
    except ImportError:
        pass
    vendor_path = "/session/metadata/vendor"
    assert vendor_path not in sys.path
