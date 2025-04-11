# WARNING: THIS FILE IS AUTOGENERATED BY update-deps.py DO NOT EDIT

load("@//:build/http.bzl", "http_archive")

TAG_NAME = "0.11.5"
URL = "https://github.com/astral-sh/ruff/releases/download/0.11.5/ruff-x86_64-unknown-linux-gnu.tar.gz"
STRIP_PREFIX = "ruff-x86_64-unknown-linux-gnu"
SHA256 = "291972831e6dc576caaa393ed4a8ed0c4d82e0ff4ea08d4e849d6e5ac840a833"
TYPE = "tgz"

def dep_ruff_linux_amd64():
    http_archive(
        name = "ruff-linux-amd64",
        url = URL,
        strip_prefix = STRIP_PREFIX,
        type = TYPE,
        sha256 = SHA256,
        build_file_content = "filegroup(name='file', srcs=['ruff'], visibility=['//visibility:public'])",
    )
