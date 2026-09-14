load("//src/workerd/server/tests/python:py_wd_test.bzl", "python_test_setup")
load(":vendor_test.bzl", "vendored_py_wd_test")

python_test_setup()

vendored_py_wd_test(
    "fastapi",
    make_snapshot = True,
    python_flags = ["0.28.2"],
    use_snapshot = "baseline",
)

vendored_py_wd_test("beautifulsoup4")

vendored_py_wd_test(
    "python-workers-runtime-sdk",
    feature_flags = ["enable_python_external_sdk"],
)

vendored_py_wd_test(
    "python-workers-runtime-sdk-override",
    feature_flags = ["disable_python_external_sdk"],
    main_py_file = "python-workers-runtime-sdk.py",
    test_template = "python-workers-runtime-sdk_vendor.wd-test",
    vendored_package_name = "python-workers-runtime-sdk",
)

vendored_py_wd_test(
    "numpy",
    make_snapshot = True,
    python_flags = [
        "0.28.2",
        "314.0.6",
    ],
    use_snapshot = "baseline",
)

# vendored_py_wd_test("scipy")

vendored_py_wd_test(
    "shapely",
    make_snapshot = True,
    python_flags = ["0.28.2"],
    use_snapshot = "baseline",
)

# Note: the `existing_dedicated` tests verify that a dedicated snapshot which has been deployed to
# production continues to work. Because of this the snapshot should never be changed and should
# remain the same to ensure Workers in production continue to work.
vendored_py_wd_test(
    "existing_dedicated_fastapi",
    feature_flags = ["python_dedicated_snapshot"],
    make_snapshot = False,
    python_flags = ["0.28.2"],
    use_snapshot = "dedicated_fastapi",
    vendored_package_name = "fastapi",
)

# The dedicated numpy snapshot is generated from numpy_vendor.wd-test by
# src/pyodide/make_snapshots.py, so this test reuses the same template and main module.
vendored_py_wd_test(
    "existing_dedicated_numpy",
    feature_flags = ["python_dedicated_snapshot"],
    main_py_file = "numpy.py",
    make_snapshot = False,
    # 0.26.0a2 does not support dedicated snapshots.
    skip_python_flags = ["0.26.0a2"],
    test_template = "numpy_vendor.wd-test",
    use_snapshot = "dedicated_numpy_vendor",
    vendored_package_name = "numpy",
)

exports_files(["generate_modules.py"])
