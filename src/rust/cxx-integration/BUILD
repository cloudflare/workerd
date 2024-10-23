load("//:build/wd_rust_crate.bzl", "rust_cxx_include", "wd_rust_crate")

wd_rust_crate(
    name = "cxx-integration",
    cxx_bridge_deps = [
        "@capnp-cpp//src/kj",
    ],
    cxx_bridge_src = "lib.rs",
    visibility = ["//visibility:public"],
    deps = [
        "@crates_vendor//:tokio",
        "@crates_vendor//:tracing",
    ],
)

rust_cxx_include(
    name = "cxx-include",
    include_prefix = "rust",
    visibility = ["//visibility:public"],
)
