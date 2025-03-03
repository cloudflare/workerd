load("//:build/wd_rust_binary.bzl", "wd_rust_binary")

wd_rust_binary(
    name = "rjs",
    cxx_bridge_src = "lib.rs",
    visibility = ["//visibility:public"],
    deps = [
        "@crates_vendor//:v8",
        "@workerd-v8//:v8",
    ],
)
