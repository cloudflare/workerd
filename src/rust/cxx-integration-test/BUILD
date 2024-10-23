# load("//:build/kj_test.bzl", "kj_test")
# load("//:build/wd_rust_crate.bzl", "wd_rust_crate")

# wd_rust_crate(
#     name = "cxx-integration-test",
#     cxx_bridge_src = "lib.rs",
#     test_deps = [
#         "@crates_vendor//:nix",
#     ],
#     visibility = ["//visibility:public"],
#     deps = [
#         "//src/rust/cxx-integration",
#         "@capnp-cpp//src/kj",
#         "@crates_vendor//:tokio",
#         "@crates_vendor//:tracing",
#     ],
# )

# kj_test(
#     src = "cxx-rust-integration-test.c++",
#     deps = [
#         ":cxx-integration-test",
#         "//src/rust/cxx-integration",
#         "@capnp-cpp//src/kj:kj-async",
#     ],
# )
