workspace(name = "workerd")

load("@//build/deps:gen/build_deps.bzl", build_deps_gen = "deps_gen")
load("@bazel_tools//tools/build_defs/repo:local.bzl", "new_local_repository")

build_deps_gen()

load("@//build/deps:gen/shared_deps.bzl", shared_deps_gen = "deps_gen")

shared_deps_gen()

load("@//build/deps:gen/deps.bzl", "deps_gen")

deps_gen()

# # ========================================================================================
# # Rust bootstrap

# load("//:build/rust_toolchains.bzl", "rust_toolchains")

# rust_toolchains()

# # rust-based lolhtml dependency, including the API header.
# # Presented as a separate repository to allow overrides.
# new_local_repository(
#     name = "com_cloudflare_lol_html",
#     build_file = "@workerd//deps/rust:BUILD.lolhtml",
#     path = "empty",
# )

# load("//build/deps:dep_pyodide.bzl", "dep_pyodide")

# dep_pyodide()
