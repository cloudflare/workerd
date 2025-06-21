load("//:build/kj_test.bzl", "kj_test")
load("//:build/wd_cc_library.bzl", "wd_cc_library")
load("//:build/wd_rust_crate.bzl", "rust_cxx_include", "wd_rust_crate")

wd_cc_library(
    name = "cxx-bridge",
    srcs = [
        "cxx-bridge.c++",
    ],
    hdrs = [
        "cxx-bridge.h",
    ],
    linkstatic = True,
    deps = [
        "//src/rust/cxx-integration",
        "@capnp-cpp//src/kj",
    ],
)

wd_rust_crate(
    name = "python-parser",
    cxx_bridge_src = "lib.rs",
    visibility = ["//visibility:public"],
    deps = [
        ":cxx-bridge",
        "@crates_vendor//:ruff_python_ast",
        "@crates_vendor//:ruff_python_parser",
    ],
)

kj_test(
    src = "import-parsing-test.c++",
    deps = [
        ":python-parser",
        "//deps/rust:runtime",
        "//src/rust/cxx-integration",
    ],
)
