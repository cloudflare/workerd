workspace(name = "workerd")

load("@bazel_tools//tools/build_defs/repo:local.bzl", "new_local_repository")

# ========================================================================================
# Rust bootstrap

load("//:build/http.bzl", "http_archive")

http_archive(
    name = "rules_rust",
    sha256 = "c8aa806cf6066679ac23463241ee80ad692265dad0465f51111cbbe30b890352",
    strip_prefix = "",
    type = "tgz",
    url = "https://github.com/bazelbuild/rules_rust/releases/download/0.68.1/rules_rust-0.68.1.tar.gz",
)

http_archive(
    name = "workerd-cxx",
    repo_mapping = {"@crates.io": "@crates_vendor"},
    sha256 = "7ddce8e81d0b81adf6f07f18376a6fea9dca42e6b03b2fcf703c62196c270ad0",
    strip_prefix = "cloudflare-workerd-cxx-916f0e7",
    type = "tgz",
    url = "https://github.com/cloudflare/workerd-cxx/tarball/916f0e7be8f1d43fe5ece1b72edd3c5844243d7b",
)

load("//:build/rust_toolchains.bzl", "rust_toolchains")

rust_toolchains()

# rust-based lolhtml dependency, including the API header.
# Presented as a separate repository to allow overrides.
new_local_repository(
    name = "com_cloudflare_lol_html",
    build_file = "@workerd//deps/rust:BUILD.lolhtml",
    path = "empty",
)

load("//build/deps:dep_pyodide.bzl", "dep_pyodide")

dep_pyodide()
