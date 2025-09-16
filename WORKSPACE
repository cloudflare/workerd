workspace(name = "workerd")

load("@//build/deps:gen/build_deps.bzl", build_deps_gen = "deps_gen")

build_deps_gen()

load("@//build/deps:gen/shared_deps.bzl", shared_deps_gen = "deps_gen")

shared_deps_gen()

load("@//build/deps:gen/deps.bzl", "deps_gen")

deps_gen()

# ========================================================================================
# Bazel basics

load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")
load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

NODE_VERSION = "22.18.0"

# ========================================================================================
# Simple dependencies

http_archive(
    name = "sqlite3",
    build_file = "//:build/BUILD.sqlite3",
    patch_args = ["-p1"],
    patches = [
        "//:patches/sqlite/0001-row-counts-plain.patch",
        "//:patches/sqlite/0002-macOS-missing-PATH-fix.patch",
        "//:patches/sqlite/0003-sqlite-complete-early-exit.patch",
        "//:patches/sqlite/0004-invalid-wal-on-rollback-fix.patch",
    ],
    sha256 = "f59c349bedb470203586a6b6d10adb35f2afefa49f91e55a672a36a09a8fedf7",
    strip_prefix = "sqlite-src-3470000",
    url = "https://sqlite.org/2024/sqlite-src-3470000.zip",
)

load("@rules_python//python:repositories.bzl", "py_repositories", "python_register_toolchains")

py_repositories()

# These are part of what's needed to get `bazel query 'deps(//...)'`, to work, but this is difficult to support
# based on our dependencies – just use cquery instead.
# load("@com_google_benchmark//:bazel/benchmark_deps.bzl", "benchmark_deps")
# benchmark_deps()

http_archive(
    name = "nbytes",
    build_file = "//:build/BUILD.nbytes",
    sha256 = "34be48071c86add2f8d14fd4a238c47230965fd743a51b8a1dd0b2f0210f0171",
    strip_prefix = "nbytes-0.1.1",
    url = "https://github.com/nodejs/nbytes/archive/refs/tags/v0.1.1.tar.gz",
)

http_archive(
    name = "ncrypto",
    sha256 = "b438cf71b1c24036e388f191a348cdc76aca75310eabca0fef5d81d5032a5d20",
    strip_prefix = "ncrypto-1.0.1",
    type = "tgz",
    url = "https://github.com/nodejs/ncrypto/archive/refs/tags/1.0.1.tar.gz",
)

# ========================================================================================
# tcmalloc
http_archive(
    name = "com_google_tcmalloc",
    integrity = "sha256-29cSZUwbEyiW8Y7FneaAzNNYLHeBmAPqBuIciHeE/u0=",
    repo_mapping = {"@com_google_absl": "@abseil-cpp"},
    strip_prefix = "google-tcmalloc-cf3dc2d",
    type = "tgz",
    url = "https://github.com/google/tcmalloc/tarball/cf3dc2d98bd64cb43f4f98db0acaf5028a7b81eb",
)

git_repository(
    name = "dragonbox",
    build_file_content = """cc_library(
            name = "dragonbox",
            hdrs = glob(["include/dragonbox/*.h"]),
            visibility = ["//visibility:public"],
            include_prefix = "third_party/dragonbox/src",
        )""",
    commit = "6c7c925b571d54486b9ffae8d9d18a822801cbda",
    remote = "https://github.com/jk-jeon/dragonbox.git",
)

git_repository(
    name = "fast_float",
    build_file_content = """cc_library(
            name = "fast_float",
            hdrs = glob(["include/fast_float/*.h"]),
            visibility = ["//visibility:public"],
            include_prefix = "third_party/fast_float/src",
        )""",
    commit = "cb1d42aaa1e14b09e1452cfdef373d051b8c02a4",
    remote = "https://github.com/fastfloat/fast_float.git",
)

git_repository(
    name = "fp16",
    build_file_content = "exports_files(glob([\"**\"]))",
    commit = "b3720617faf1a4581ed7e6787cc51722ec7751f0",
    remote = "https://github.com/Maratyszcza/FP16.git",
)

git_repository(
    name = "highway",
    commit = "00fe003dac355b979f36157f9407c7c46448958e",
    remote = "https://github.com/google/highway.git",
)

# ========================================================================================
# Node.js bootstrap
#
# workerd uses Node.js scripts for generating TypeScript types.

load("@aspect_rules_js//js:repositories.bzl", "rules_js_dependencies")

rules_js_dependencies()

load("@aspect_rules_js//js:toolchains.bzl", "rules_js_register_toolchains")

rules_js_register_toolchains(
    node_urls = [
        # github workflows may substitute a mirror URL here to avoid fetch failures.
        # "WORKERS_MIRROR_URL/https://nodejs.org/dist/v{version}/{filename}",
        "https://nodejs.org/dist/v{version}/{filename}",
    ],
    node_version = NODE_VERSION,
)

load("@aspect_rules_ts//ts:repositories.bzl", "rules_ts_dependencies")

rules_ts_dependencies(ts_version_from = "//:package.json")

load("@aspect_rules_js//npm:repositories.bzl", "npm_translate_lock")

npm_translate_lock(
    name = "npm",
    npmrc = "//:.npmrc",
    pnpm_lock = "//:pnpm-lock.yaml",
)

load("@npm//:repositories.bzl", "npm_repositories")

npm_repositories()

load("@aspect_rules_esbuild//esbuild:dependencies.bzl", "rules_esbuild_dependencies")

rules_esbuild_dependencies()

load("@aspect_rules_esbuild//esbuild:repositories.bzl", "LATEST_ESBUILD_VERSION", "esbuild_register_toolchains")

esbuild_register_toolchains(
    name = "esbuild",
    esbuild_version = LATEST_ESBUILD_VERSION,
)

load("@//build/deps:v8.bzl", "deps_v8")

deps_v8()

PYTHON_TOOLCHAIN = "python3_13"

PYTHON_INTERPRETER = "@" + PYTHON_TOOLCHAIN + "_host//:python"

python_register_toolchains(
    name = PYTHON_TOOLCHAIN,
    ignore_root_user_error = True,
    # https://github.com/bazelbuild/rules_python/blob/main/python/versions.bzl
    python_version = "3.13",
    register_coverage_tool = True,
)

load("@rules_python//python:pip.bzl", "pip_parse")

pip_parse(
    name = "v8_python_deps",
    extra_pip_args = ["--require-hashes"],
    python_interpreter_target = PYTHON_INTERPRETER,
    requirements_lock = "@v8//:bazel/requirements.txt",
)

load("@v8_python_deps//:requirements.bzl", v8_python_deps_install = "install_deps")

v8_python_deps_install()

pip_parse(
    name = "py_deps",
    python_interpreter_target = PYTHON_INTERPRETER,
    requirements_lock = "//build/deps:requirements.txt",
)

load("@py_deps//:requirements.bzl", py_deps_install = "install_deps")

py_deps_install()

bind(
    name = "icu",
    actual = "@com_googlesource_chromium_icu//:icu",
)

# Tell workerd code where to find v8.
#
# We indirect through `@workerd-v8` to allow dependents to override how and where `v8` is built.
#
# TODO(cleanup): There must be a better way to do this?
# TODO(soon): Figure out how to build v8 with perfetto enabled. It does not appear
#             as if the v8 bazel build currently includes support for building with
#             perfetto enabled as an option.
new_local_repository(
    name = "workerd-v8",
    build_file_content = """cc_library(
        name = "v8",
        defines = ["WORKERD_ICU_DATA_EMBED"],
        deps = [ "@v8//:v8_icu", "@workerd//:icudata-embed" ],
        visibility = ["//visibility:public"])""",
    path = "empty",
)

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
