load("//:build/kj_test.bzl", "kj_test")
load("//:build/wd_rust_crate.bzl", "rust_cxx_include", "wd_rust_crate")

wd_rust_crate(
    name = "python-parser",
    cxx_bridge_src = "lib.rs",
    visibility = ["//visibility:public"],
    deps = [
        "@crates_vendor//:ruff_python_ast",
        "@crates_vendor//:ruff_python_parser",
    ],
)

kj_test(
    src = "import_parsing.c++",
    deps = [
        ":python-parser",
        "//deps/rust:runtime",
        "//src/rust/cxx-integration",
    ],
)
