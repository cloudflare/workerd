workspace(name = "workerd")

load("@//build/deps:gen/build_deps.bzl", build_deps_gen = "deps_gen")

build_deps_gen()

load("@//build/deps:gen/shared_deps.bzl", shared_deps_gen = "deps_gen")

shared_deps_gen()

load("@//build/deps:gen/deps.bzl", "deps_gen")

deps_gen()

# ========================================================================================
# Rust bootstrap

load("//:build/rust_toolchains.bzl", "rust_toolchains")

rust_toolchains()

# rust-based lolhtml dependency, including the API header.
# Presented as a separate repository to allow overrides.
new_local_repository(
    name = "com_cloudflare_lol_html",
    build_file = "@workerd//deps/rust:BUILD.lolhtml",
    path = "empty",
)

# Use @workerd prefix on build_file so we can use this from edgeworker too
# Note: this one is a raw http_archive so the repository gets the old WORKSPACE-style canonical name.
# Otherwise we have the bzlmod apparent name to deal with, which has plus characters and makes Windows testing harder.
load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

[
    http_archive(
        name = "all_pyodide_wheels_%s" % tag,
        build_file = "@workerd//:build/BUILD.all_pyodide_wheels",
        sha256 = all_wheels_hash,
        urls = ["https://github.com/cloudflare/pyodide-build-scripts/releases/download/%s/all_wheels.zip" % tag],
    )
    # FIXME(alexeagle): Keep in sync
    for tag, all_wheels_hash in [
        ("20240829.4", "94653dc8cfbea62b8013db3b8584bc02544ad6fc647b0d83bdee5dfcda5d4b62"),
        ("20250808", "7228cf17e569e31238f74b00e4cb702f0b4fc1fa55e6a5144be461e75240048b"),
    ]
]
