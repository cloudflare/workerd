workspace(name = "workerd")

# ========================================================================================
# Rust bootstrap

load("//:build/http.bzl", "http_archive")

http_archive(
    name = "workerd-cxx",
    repo_mapping = {"@crates.io": "@crates_vendor"},
    sha256 = "7ddce8e81d0b81adf6f07f18376a6fea9dca42e6b03b2fcf703c62196c270ad0",
    strip_prefix = "cloudflare-workerd-cxx-916f0e7",
    type = "tgz",
    url = "https://github.com/cloudflare/workerd-cxx/tarball/916f0e7be8f1d43fe5ece1b72edd3c5844243d7b",
)

load("//build/deps:dep_pyodide.bzl", "dep_pyodide")

dep_pyodide()
