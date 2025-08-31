load("//:build/wd_rust_crate.bzl", "wd_rust_crate")

wd_rust_crate(
    name = "transpiler",
    cxx_bridge_src = "lib.rs",
    visibility = ["//visibility:public"],
    deps = [
        "//src/rust/cxx-integration",
        "@crates_vendor//:swc_core",
        "@crates_vendor//:swc_ts_fast_strip",
        "@crates_vendor//:thiserror",
    ],
)
