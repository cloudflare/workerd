# WARNING: THIS FILE IS AUTOGENERATED BY update-deps.py DO NOT EDIT

load("@//:build/http.bzl", "http_file")

TAG_NAME = "v8.0.3"
URL = "https://github.com/bazelbuild/buildtools/releases/download/v8.0.3/buildifier-darwin-amd64"
SHA256 = "b7a3152cde0b3971b1107f2274afe778c5c154dcdf6c9c669a231e3c004f047e"

def dep_buildifier_darwin_amd64():
    http_file(
        name = "buildifier-darwin-amd64",
        url = URL,
        executable = True,
        sha256 = SHA256,
    )
