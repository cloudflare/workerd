import sys

from a import A


def test():
    assert A == 77
    test_path_ordering()


def test_path_ordering():
    """Verify that the Python path is set in the correct order.
    '/session/metadata/python_modules' should be positioned after the system paths
    but before any site-packages paths.
    """
    vendor_path = "/session/metadata/python_modules"
    vendor_index = sys.path.index(vendor_path)

    # Check that vendor_path is in sys.path
    assert vendor_index >= 0, f"{vendor_path} not found in sys.path"

    # Verify that all site-packages paths come after the vendor path
    for i, path in enumerate(sys.path):
        if "site-packages" in path:
            assert i > vendor_index, (
                f"site-packages path {path} appears before vendor path"
            )

    # Verify system paths come before the vendor path
    system_paths = [
        "/lib/python312.zip",
        "/lib/python3.12",
        "/lib/python3.12/lib-dynload",
    ]
    for path in system_paths:
        if path in sys.path:
            assert sys.path.index(path) < vendor_index, (
                f"System path {path} appears after vendor path"
            )

    # Print the path for debugging if needed
    print("Python sys.path:", sys.path)
