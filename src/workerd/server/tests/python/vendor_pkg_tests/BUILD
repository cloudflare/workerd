load(":vendor_test.bzl", "vendored_py_wd_test")

vendored_py_wd_test(
    name = "fastapi_vendor_test",
    main_py_file = "fastapi.py",
    test_template = "fastapi_vendor.wd-test",
    vendored_srcs_target_prefix = "@fastapi_src",
)

vendored_py_wd_test(
    name = "beautifulsoup4_vendor_test",
    main_py_file = "beautifulsoup4.py",
    test_template = "beautifulsoup4_vendor.wd-test",
    vendored_srcs_target_prefix = "@beautifulsoup4_src",
)
