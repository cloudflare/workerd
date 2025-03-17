load("//:build/wd_rust_crate.bzl", "wd_rust_crate")

wd_rust_crate(
    name = "rjs",
    cxx_bridge_src = "lib.rs",
    visibility = ["//visibility:public"],
    deps = [
        "@crates_vendor//:v8",
        "@workerd-v8//:v8",
    ],
)
