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
    python_modules_path = "/session/metadata/python_modules"
    python_modules_index = sys.path.index(python_modules_path)

    # Check that vendor_path is in sys.path
    assert python_modules_index >= 0, f"{python_modules_path} not found in sys.path"

    # Verify that all site-packages paths come after the vendor path
    for i, path in enumerate(sys.path):
        if "site-packages" in path:
            assert i > python_modules_index, (
                f"site-packages path {path} appears before python_modules path"
            )

    # Verify system paths come before the vendor path
    system_paths = [
        "/lib/python312.zip",
        "/lib/python3.12",
        "/lib/python3.12/lib-dynload",
    ]
    for path in system_paths:
        if path in sys.path:
            assert sys.path.index(path) < python_modules_index, (
                f"System path {path} appears after python_modules path"
            )

    # Print the path for debugging if needed
    print("Python sys.path:", sys.path)
