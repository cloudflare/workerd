# WARNING: THIS FILE IS AUTOGENERATED BY update-deps.py DO NOT EDIT

load("@//:build/http.bzl", "http_archive")

TAG_NAME = "v3.2.2"
URL = "https://github.com/ada-url/ada/releases/download/v3.2.2/singleheader.zip"
STRIP_PREFIX = ""
SHA256 = "caae8ecdb96fd4a50828205a327b62d047a028f08cd370b74178e23903140831"
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
