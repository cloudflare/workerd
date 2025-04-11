# WARNING: THIS FILE IS AUTOGENERATED BY update-deps.py DO NOT EDIT

load("@//:build/http.bzl", "http_archive")

TAG_NAME = "1.21.1"
URL = "https://github.com/bazelbuild/apple_support/releases/download/1.21.1/apple_support.1.21.1.tar.gz"
STRIP_PREFIX = "./"
SHA256 = "73c8dc6cdd7cea87956db9279a69c9e68bd2a5ec6a6a507e36d6e2d7da4d71a4"
TYPE = "tgz"

def dep_build_bazel_apple_support():
    http_archive(
        name = "build_bazel_apple_support",
        url = URL,
        strip_prefix = STRIP_PREFIX,
        type = TYPE,
        sha256 = SHA256,
    )
