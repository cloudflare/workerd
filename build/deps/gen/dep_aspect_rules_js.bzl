# WARNING: THIS FILE IS AUTOGENERATED BY update-deps.py DO NOT EDIT

load("@//:build/http.bzl", "http_archive")

TAG_NAME = "v2.3.5"
URL = "https://github.com/aspect-build/rules_js/releases/download/v2.3.5/rules_js-v2.3.5.tar.gz"
STRIP_PREFIX = "rules_js-2.3.5"
SHA256 = "1be1a3ec3d3baec4a71bc09ce446eb59bb48ae31af63016481df1532a0d81aee"
TYPE = "tgz"

def dep_aspect_rules_js():
    http_archive(
        name = "aspect_rules_js",
        url = URL,
        strip_prefix = STRIP_PREFIX,
        type = TYPE,
        sha256 = SHA256,
    )
