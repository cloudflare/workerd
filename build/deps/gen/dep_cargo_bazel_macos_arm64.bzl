# WARNING: THIS FILE IS AUTOGENERATED BY update-deps.py DO NOT EDIT

load("@//:build/http.bzl", "http_file")

TAG_NAME = "0.51.0"
URL = "https://github.com/bazelbuild/rules_rust/releases/download/0.51.0/cargo-bazel-aarch64-apple-darwin"
SHA256 = "604190df2967941f735c3b9a6a0ba92e6c826379b0a49bb00ec9cf61af59392d"

def dep_cargo_bazel_macos_arm64():
    http_file(
        name = "cargo_bazel_macos_arm64",
        url = URL,
        executable = True,
        sha256 = SHA256,
    )
