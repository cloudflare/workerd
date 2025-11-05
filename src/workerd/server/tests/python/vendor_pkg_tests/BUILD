load("//src/workerd/server/tests/python:py_wd_test.bzl", "python_test_setup")
load(":vendor_test.bzl", "vendored_py_wd_test")

python_test_setup()

vendored_py_wd_test("fastapi")

vendored_py_wd_test("beautifulsoup4")

vendored_py_wd_test(
    "python-workers-runtime-sdk",
)

# vendored_py_wd_test("scipy")

vendored_py_wd_test(
    "shapely",
    feature_flags = ["python_dedicated_snapshot"],
    make_snapshot = True,
    python_flags = ["0.28.2"],
    use_snapshot = "baseline",
)
