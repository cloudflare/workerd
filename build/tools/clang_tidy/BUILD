load("@rules_python//python:defs.bzl", "py_binary")

exports_files(["clang_tidy_wrapper.sh"])

py_binary(
    name = "merge_clang_tidy_configs",
    srcs = ["merge_clang_tidy_configs.py"],
    visibility = ["//visibility:public"],
)
