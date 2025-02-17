# WARNING: THIS FILE IS AUTOGENERATED BY update-deps.py DO NOT EDIT

load("@//:build/http.bzl", "http_archive")

TAG_NAME = "v3.1.0"
URL = "https://github.com/ada-url/ada/releases/download/v3.1.0/singleheader.zip"
STRIP_PREFIX = ""
SHA256 = "54de2c093a94ab0b6ffc3c16d01a89ef7cacbd743f08b5fa63fd06b8684efbd6"
TYPE = "zip"

def dep_ada_url():
    http_archive(
        name = "ada-url",
        url = URL,
        strip_prefix = STRIP_PREFIX,
        type = TYPE,
        sha256 = SHA256,
        build_file = "//:build/BUILD.ada-url",
    )
