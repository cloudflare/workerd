# WARNING: THIS FILE IS AUTOGENERATED BY update-deps.py DO NOT EDIT

load("@//:build/http.bzl", "http_file")

TAG_NAME = "0.60.0"
URL = "https://github.com/bazelbuild/rules_rust/releases/download/0.60.0/cargo-bazel-aarch64-unknown-linux-gnu"
SHA256 = "47ac0bcf1475f7fb6238c67a474314ae8ee78c463fd87ac631a0fde863e197b3"

def dep_cargo_bazel_linux_arm64():
    http_file(
        name = "cargo_bazel_linux_arm64",
        url = URL,
        executable = True,
        sha256 = SHA256,
    )
