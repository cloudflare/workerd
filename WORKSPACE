workspace(name = "workerd")

load("@//build/deps:gen/build_deps.bzl", build_deps_gen = "deps_gen")

build_deps_gen()

load("@//build/deps:gen/shared_deps.bzl", shared_deps_gen = "deps_gen")

shared_deps_gen()

load("@//build/deps:gen/deps.bzl", "deps_gen")

deps_gen()

# Tell workerd code where to find google-benchmark with CodSpeed.
#
# We indirect through `@workerd-google-benchmark` to allow dependents to override how and where
# google-benchmark is built, similar to the v8 setup above.
new_local_repository(
    name = "workerd-google-benchmark",
    build_file_content = """cc_library(
        name = "benchmark",
        deps = [ "@codspeed//google_benchmark:benchmark" ],
        visibility = ["//visibility:public"])

cc_library(
        name = "benchmark_main",
        deps = [ "@codspeed//google_benchmark:benchmark_main" ],
        visibility = ["//visibility:public"])""",
    path = "empty",
)

# rust-based lolhtml dependency, including the API header.
# Presented as a separate repository to allow overrides.
new_local_repository(
    name = "com_cloudflare_lol_html",
    build_file = "@workerd//deps/rust:BUILD.lolhtml",
    path = "empty",
)

load("//build/deps:dep_pyodide.bzl", "dep_pyodide")

dep_pyodide()
