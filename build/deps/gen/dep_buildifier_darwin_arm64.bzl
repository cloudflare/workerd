# WARNING: THIS FILE IS AUTOGENERATED BY update-deps.py DO NOT EDIT

load("@//:build/http.bzl", "http_file")

TAG_NAME = "v8.0.1"
URL = "https://github.com/bazelbuild/buildtools/releases/download/v8.0.1/buildifier-darwin-arm64"
SHA256 = "833e2afc331b9ad8f6b038ad3d69ceeaf97651900bf2a3a45f54f42cafe0bfd3"

def dep_buildifier_darwin_arm64():
    http_file(
        name = "buildifier-darwin-arm64",
        url = URL,
        executable = True,
        sha256 = SHA256,
    )
