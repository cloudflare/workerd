workspace(name = "workerd")

load("@//build/deps:gen/build_deps.bzl", build_deps_gen = "deps_gen")

build_deps_gen()

load("@//build/deps:gen/deps.bzl", "deps_gen")

deps_gen()

# ========================================================================================
# Bazel basics

load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")
load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive", "http_file")

NODE_VERSION = "20.14.0"

load("@bazel_skylib//:workspace.bzl", "bazel_skylib_workspace")

bazel_skylib_workspace()

load(
    "@build_bazel_apple_support//lib:repositories.bzl",
    "apple_support_dependencies",
)

apple_support_dependencies()

# apple_support now requires bazel_features, pull in its dependencies too.
load("@bazel_features//:deps.bzl", "bazel_features_deps")

bazel_features_deps()

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
    ],
    sha256 = "ab9aae38a11b931f35d8d1c6d62826d215579892e6ffbf89f20bdce106a9c8c5",
    strip_prefix = "sqlite-src-3440000",
    url = "https://sqlite.org/2023/sqlite-src-3440000.zip",
)

load("@rules_python//python:repositories.bzl", "py_repositories", "python_register_toolchains")

py_repositories()

http_archive(
    name = "com_google_benchmark",
    integrity = "sha256-a8GApX0j1NlRVRn5KwyD1hsFtbqxiJYfNqx7BrDZ6c4=",
    strip_prefix = "benchmark-1.8.3",
    url = "https://github.com/google/benchmark/archive/refs/tags/v1.8.3.tar.gz",
)

# These are part of what's needed to get `bazel query 'deps(//...)'`, to work, but this is difficult to support
# based on our dependencies – just use cquery instead.
# load("@com_google_benchmark//:bazel/benchmark_deps.bzl", "benchmark_deps")
# benchmark_deps()

http_archive(
    name = "brotli",
    sha256 = "e720a6ca29428b803f4ad165371771f5398faba397edf6778837a18599ea13ff",
    strip_prefix = "brotli-1.1.0",
    urls = ["https://github.com/google/brotli/archive/refs/tags/v1.1.0.tar.gz"],
)

http_archive(
    name = "ada-url",
    build_file = "//:build/BUILD.ada-url",
    sha256 = "30d4f4cccbd8b0455a71a2180da95f3a38d085e4a440eb931da94c7272705edc",
    type = "zip",
    url = "https://github.com/ada-url/ada/releases/download/v2.9.1/singleheader.zip",
)

http_archive(
    name = "nbytes",
    build_file = "//:build/BUILD.nbytes",
    sha256 = "34be48071c86add2f8d14fd4a238c47230965fd743a51b8a1dd0b2f0210f0171",
    strip_prefix = "nbytes-0.1.1",
    url = "https://github.com/nodejs/nbytes/archive/refs/tags/v0.1.1.tar.gz",
)

http_archive(
    name = "simdutf",
    build_file = "//:build/BUILD.simdutf",
    sha256 = "7867c118a11bb7ccaea0f999a28684b06040027506b424b706146cc912b80ff6",
    type = "zip",
    url = "https://github.com/simdutf/simdutf/releases/download/v5.2.8/singleheader.zip",
)

http_archive(
    name = "pyodide",
    build_file = "//:build/BUILD.pyodide",
    sha256 = "fbda450a64093a8d246c872bb901ee172a57fe594c9f35bba61f36807c73300d",
    urls = ["https://github.com/pyodide/pyodide/releases/download/0.26.0a2/pyodide-core-0.26.0a2.tar.bz2"],
)

http_archive(
    name = "pyodide_packages",
    build_file = "//:build/BUILD.pyodide_packages",
    sha256 = "c4a4e0c1cb658a39abc0435cc07df902e5a2ecffc091e0528b96b0c295e309ea",
    urls = ["https://github.com/dom96/pyodide_packages/releases/download/just-stdlib/pyodide_packages.tar.zip"],
)

load("//:build/pyodide_bucket.bzl", "PYODIDE_ALL_WHEELS_ZIP_SHA256", "PYODIDE_GITHUB_RELEASE_URL", "PYODIDE_LOCK_SHA256")

http_file(
    name = "pyodide-lock.json",
    sha256 = PYODIDE_LOCK_SHA256,
    url = PYODIDE_GITHUB_RELEASE_URL + "pyodide-lock.json",
)

http_archive(
    name = "all_pyodide_wheels",
    build_file_content = """
filegroup(
    name = "whls",
    srcs = glob(["*"]),
    visibility = ["//visibility:public"]
)
    """,
    sha256 = PYODIDE_ALL_WHEELS_ZIP_SHA256,
    urls = [PYODIDE_GITHUB_RELEASE_URL + "all_wheels.zip"],
)

# ========================================================================================
# tcmalloc

# tcmalloc requires Abseil.
#
# WARNING: This MUST appear before rules_fuzzing_dependencies(), below. Otherwise,
#   rules_fuzzing_dependencies() will choose to pull in an older version of Abseil. Absurdly, Bazel
#   simply ignores later attempts to define the same repo name, rather than erroring out. This led
#   to confusing compiler errors in tcmalloc in the past.
git_repository(
    name = "com_google_absl",
    commit = "ed3733b91e472a1e7a641c1f0c1e6c0ea698e958",
    remote = "https://chromium.googlesource.com/chromium/src/third_party/abseil-cpp.git",
)

git_repository(
    name = "fp16",
    build_file_content = "exports_files(glob([\"**\"]))",
    commit = "0a92994d729ff76a58f692d3028ca1b64b145d91",
    remote = "https://chromium.googlesource.com/external/github.com/Maratyszcza/FP16.git",
)

# Bindings for abseil libraries used by V8
[
    bind(
        name = "absl_" + absl_component,
        actual = "@com_google_absl//absl/container:" + absl_component,
    )
    for absl_component in [
        "btree",
        "flat_hash_map",
        "flat_hash_set",
    ]
]

bind(
    name = "absl_optional",
    actual = "@com_google_absl//absl/types:optional",
)

# tcmalloc requires this "rules_fuzzing" package. Its build files fail analysis without it, even
# though it is unused for our purposes.
http_archive(
    name = "rules_fuzzing",
    integrity = "sha256-PsDu4FskNVLMSnhLMDI9CIv3PLIXfd2gLIJ+aJgZM/E=",
    strip_prefix = "rules_fuzzing-0.5.2",
    url = "https://github.com/bazelbuild/rules_fuzzing/archive/refs/tags/v0.5.2.tar.gz",
)

load("@rules_fuzzing//fuzzing:repositories.bzl", "rules_fuzzing_dependencies")

rules_fuzzing_dependencies(
    honggfuzz = False,
    jazzer = False,
)

load("@rules_fuzzing//fuzzing:init.bzl", "rules_fuzzing_init")

rules_fuzzing_init()

# OK, now we can bring in tcmalloc itself.
http_archive(
    name = "com_google_tcmalloc",
    sha256 = "81f285cb337f445276f37c308cb90120f8ba4311d1be9daf3b93dccf4bfdba7d",
    strip_prefix = "google-tcmalloc-69c409c",
    type = "tgz",
    url = "https://github.com/google/tcmalloc/tarball/69c409c344bdf894fc7aab83e2d9e280b009b2f3",
)

# ========================================================================================
# Rust bootstrap
#

git_repository(
    name = "zlib",
    build_file = "//:build/BUILD.zlib",
    # Must match the version used by v8
    commit = "d3aea2341cdeaf7e717bc257a59aa7a9407d318a",
    remote = "https://chromium.googlesource.com/chromium/src/third_party/zlib.git",
)

load("@rules_rust//rust:repositories.bzl", "rules_rust_dependencies", "rust_register_toolchains")

rules_rust_dependencies()

rust_register_toolchains(
    edition = "2021",
    # Rust registers wasm targets by default which we don't need, workerd is only built for its native platform.
    extra_target_triples = [],
    versions = ["1.81.0"],  # LLVM 18
)

load("@rules_rust//crate_universe:repositories.bzl", "crate_universe_dependencies")

crate_universe_dependencies()

# Load rust crate dependencies.
# These could be regenerated from cargo.bzl by using
# `just update-rust` (consult `just --list` or justfile for more details)
load("//deps/rust/crates:crates.bzl", "crate_repositories")

crate_repositories()

load("@rules_rust//tools/rust_analyzer:deps.bzl", "rust_analyzer_dependencies")

rust_analyzer_dependencies()

# ========================================================================================
# Node.js bootstrap
#
# workerd uses Node.js scripts for generating TypeScript types.

# TODO(soon): rules_js depends on bazel-lib, which broke on Windows after a dependency binary was
# deleted. There is a fix available at https://github.com/bazel-contrib/bazel-lib/pull/940, but it
# is based off of a commit where WORKSPACE dependencies appear to be broken. Create a patch for the
# latest release build instead. Remove this ASAP once the fix has been merged and rules_js has been
# updated with the fixed version.
http_archive(
    name = "aspect_bazel_lib",
    patch_args = ["-p1"],
    patches = [
        # based on https://github.com/bazel-contrib/bazel-lib/pull/940.
        "//:patches/bazel-lib/0001-chore-deps-upgrade-to-newest-bsdtar.patch",
    ],
    sha256 = "688354ee6beeba7194243d73eb0992b9a12e8edeeeec5b6544f4b531a3112237",
    strip_prefix = "bazel-lib-2.8.1",
    url = "https://github.com/aspect-build/bazel-lib/releases/download/v2.8.1/bazel-lib-v2.8.1.tar.gz",
)

http_archive(
    name = "aspect_rules_js",
    sha256 = "6b7e73c35b97615a09281090da3645d9f03b2a09e8caa791377ad9022c88e2e6",
    strip_prefix = "rules_js-2.0.0",
    url = "https://github.com/aspect-build/rules_js/releases/download/v2.0.0/rules_js-v2.0.0.tar.gz",
)

http_archive(
    name = "aspect_rules_ts",
    sha256 = "ee7dcc35faef98f3050df9cf26f2a72ef356cab8ad927efb1c4dc119ac082a19",
    strip_prefix = "rules_ts-3.0.0",
    url = "https://github.com/aspect-build/rules_ts/releases/download/v3.0.0/rules_ts-v3.0.0.tar.gz",
)

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
    patch_args = {
        "capnp-ts@0.7.0": ["-p1"],
    },
    # Patches required for `capnp-ts` to type-check
    patches = {
        "capnp-ts@0.7.0": ["//:patches/capnp-ts@0.7.0.patch"],
    },
    pnpm_lock = "//:pnpm-lock.yaml",
)

load("@npm//:repositories.bzl", "npm_repositories")

npm_repositories()

# ========================================================================================
# V8 and its dependencies
#
# Note that googlesource does not generate tarballs deterministically, so we cannot use
# http_archive: https://github.com/google/gitiles/issues/84
#
# It would seem that googlesource would rather we use git protocol.
# Fine, we can do that.
#
# We previously used shallow_since for our git-based dependencies, but this may actually be
# harmful: https://github.com/bazelbuild/bazel/issues/12857
#
# There is an official mirror for V8 itself on GitHub, but not for dependencies like zlib (Chromium
# fork), icu (Chromium fork), and trace_event, so we still have to use git for them.

http_archive(
    name = "v8",
    integrity = "sha256-oOgRa4akl02v8tcMbpmbHu21VL1qOYBjerq1CzekLxc=",
    patch_args = ["-p1"],
    patches = [
        "//:patches/v8/0001-Allow-manually-setting-ValueDeserializer-format-vers.patch",
        "//:patches/v8/0002-Allow-manually-setting-ValueSerializer-format-versio.patch",
        "//:patches/v8/0003-Add-ArrayBuffer-MaybeNew.patch",
        "//:patches/v8/0004-Allow-Windows-builds-under-Bazel.patch",
        "//:patches/v8/0005-Disable-bazel-whole-archive-build.patch",
        "//:patches/v8/0006-Speed-up-V8-bazel-build-by-always-using-target-cfg.patch",
        "//:patches/v8/0007-Implement-Promise-Context-Tagging.patch",
        "//:patches/v8/0008-Enable-V8-shared-linkage.patch",
        "//:patches/v8/0009-Randomize-the-initial-ExecutionContextId-used-by-the.patch",
        "//:patches/v8/0010-increase-visibility-of-virtual-method.patch",
        "//:patches/v8/0011-Add-ValueSerializer-SetTreatFunctionsAsHostObjects.patch",
        "//:patches/v8/0012-Set-torque-generator-path-to-external-v8.-This-allow.patch",
        "//:patches/v8/0013-Modify-where-to-look-for-fp16-dependency.-This-depen.patch",
        "//:patches/v8/0014-Expose-v8-Symbol-GetDispose.patch",
        "//:patches/v8/0015-Rename-V8_COMPRESS_POINTERS_IN_ISOLATE_CAGE-V8_COMPR.patch",
        "//:patches/v8/0016-Revert-TracedReference-deref-API-removal.patch",
        "//:patches/v8/0017-Revert-heap-Add-masm-specific-unwinding-annotations-.patch",
        "//:patches/v8/0018-Update-illegal-invocation-error-message-in-v8.patch",
        "//:patches/v8/0019-Implement-cross-request-context-promise-resolve-hand.patch",
    ],
    strip_prefix = "v8-12.9.202.13",
    url = "https://github.com/v8/v8/archive/refs/tags/12.9.202.13.tar.gz",
)

git_repository(
    name = "com_googlesource_chromium_icu",
    build_file = "@v8//:bazel/BUILD.icu",
    commit = "9408c6fd4a39e6fef0e1c4077602e1c83b15f3fb",
    patch_cmds = ["find source -name BUILD.bazel | xargs rm"],
    patch_cmds_win = ["Get-ChildItem -Path source -File -Include BUILD.bazel -Recurse | Remove-Item"],
    remote = "https://chromium.googlesource.com/chromium/deps/icu.git",
)

http_archive(
    name = "perfetto",
    patch_args = ["-p1"],
    patches = [
        "//:patches/perfetto/0001-Rename-ui-build-to-ui-build.sh-to-allow-bazel-build-.patch",
    ],
    repo_mapping = {"@perfetto_dep_zlib": "@zlib"},
    sha256 = "241cbaddc9ff4e5d1de2d28497fef40b5510e9ca60808815bf4944d0d2f026db",
    strip_prefix = "perfetto-39.0",
    url = "https://github.com/google/perfetto/archive/refs/tags/v39.0.tar.gz",
)

# For use with perfetto
http_archive(
    name = "com_google_protobuf",
    sha256 = "6adf73fd7f90409e479d6ac86529ade2d45f50494c5c10f539226693cb8fe4f7",
    strip_prefix = "protobuf-3.10.1",
    url = "https://github.com/protocolbuffers/protobuf/archive/refs/tags/v3.10.1.tar.gz",
)

# For use with perfetto
new_local_repository(
    name = "perfetto_cfg",
    build_file_content = "",
    path = "build/perfetto",
)

python_register_toolchains(
    name = "python3_12",
    ignore_root_user_error = True,
    # https://github.com/bazelbuild/rules_python/blob/main/python/versions.bzl
    python_version = "3.12",
)

load("@python3_12//:defs.bzl", "interpreter")
load("@rules_python//python:pip.bzl", "pip_parse")

pip_parse(
    name = "v8_python_deps",
    extra_pip_args = ["--require-hashes"],
    python_interpreter_target = interpreter,
    requirements_lock = "@v8//:bazel/requirements.txt",
)

load("@v8_python_deps//:requirements.bzl", v8_python_deps_install = "install_deps")

v8_python_deps_install()

pip_parse(
    name = "py_deps",
    python_interpreter_target = interpreter,
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
        deps = [ "@v8//:v8_icu", "@workerd//:icudata-embed" ],
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

# Dev tools
http_file(
    name = "buildifier-darwin-arm64",
    executable = True,
    integrity = "sha256-Wmr8asegn1RVuguJvZnVriO0F03F3J1sDtXOjKrD+BM=",
    url = "https://github.com/bazelbuild/buildtools/releases/download/v7.3.1/buildifier-darwin-arm64",
)

http_file(
    name = "buildifier-darwin-amd64",
    executable = True,
    integrity = "sha256-Wmr8asegn1RVuguJvZnVriO0F03F3J1sDtXOjKrD+BM=",
    url = "https://github.com/bazelbuild/buildtools/releases/download/v7.3.1/buildifier-darwin-arm64",
)

http_file(
    name = "buildifier-linux-arm64",
    executable = True,
    integrity = "sha256-C/hsS//69PCO7Xe95bIILkrlA5oR4uiwOYTBc8NKVhw=",
    url = "https://github.com/bazelbuild/buildtools/releases/download/v7.3.1/buildifier-linux-arm64",
)

http_file(
    name = "buildifier-linux-amd64",
    executable = True,
    integrity = "sha256-VHTMUSinToBng9VAgfWBZixL6K5lAi9VfpKB7V3IgAk=",
    url = "https://github.com/bazelbuild/buildtools/releases/download/v7.3.1/buildifier-linux-amd64",
)

http_file(
    name = "buildifier-windows-amd64",
    executable = True,
    integrity = "sha256-NwzVdgda0pkwqC9d4TLxod5AhMeEqCUUvU2oDIWs9Kg=",
    url = "https://github.com/bazelbuild/buildtools/releases/download/v7.3.1/buildifier-windows-amd64.exe",
)

FILE_GROUP = """filegroup(
	name="file",
	srcs=["**/*"])"""

http_archive(
    name = "ruff-darwin-arm64",
    build_file_content = FILE_GROUP,
    integrity = "sha256-KbGnLDXu1bIkD/Nl4fRdB7wMQLkzGhGcK1nMpEwPMi4=",
    strip_prefix = "ruff-aarch64-apple-darwin",
    url = "https://github.com/astral-sh/ruff/releases/download/0.6.7/ruff-aarch64-apple-darwin.tar.gz",
)

http_archive(
    name = "ruff-darwin-amd64",
    build_file_content = FILE_GROUP,
    integrity = "sha256-W3JL0sldkm6kbaB9+mrFVoY32wR0CDhpi7SxkJ2Oug0=",
    strip_prefix = "ruff-x86_64-apple-darwin",
    url = "https://github.com/astral-sh/ruff/releases/download/0.6.7/ruff-x86_64-apple-darwin.tar.gz",
)

http_archive(
    name = "ruff-linux-arm64",
    build_file_content = FILE_GROUP,
    integrity = "sha256-7nBZdr686PdPmCFa2EN65OHgmDCfqB3ygFaXVgUDRuM=",
    strip_prefix = "ruff-aarch64-unknown-linux-gnu",
    url = "https://github.com/astral-sh/ruff/releases/download/0.6.7/ruff-aarch64-unknown-linux-gnu.tar.gz",
)

http_archive(
    name = "ruff-linux-amd64",
    build_file_content = FILE_GROUP,
    integrity = "sha256-Uu1+NMFYCfMT4/jtQoH+Uj5+XwZn57+ZWIhbfm8icKg=",
    strip_prefix = "ruff-x86_64-unknown-linux-gnu",
    url = "https://github.com/astral-sh/ruff/releases/download/0.6.7/ruff-x86_64-unknown-linux-gnu.tar.gz",
)

http_archive(
    name = "ruff-windows-amd64",
    build_file_content = FILE_GROUP,
    integrity = "sha256-H2yX4kuLyNdBrkRPhTr61FQqJRyiKeLq4TnMmKE0t2A=",
    url = "https://github.com/astral-sh/ruff/releases/download/0.6.7/ruff-x86_64-pc-windows-msvc.zip",
)

# clang-format static binary builds via GH Actions: https://github.com/npaun/bins/blob/master/.github/workflows/llvm.yml
# TODO(soon): Move this workflow to a repo in the cloudflare GH organization

http_file(
    name = "clang-format-darwin-arm64",
    executable = True,
    integrity = "sha256-1hG7AcfgGL+IBrSCEhD9ed6pvIpZMdXMdhCDGkqzhpA=",
    url = "https://github.com/npaun/bins/releases/download/llvm-18.1.8/llvm-18.1.8-darwin-arm64-clang-format",
)

http_file(
    name = "clang-format-linux-arm64",
    executable = True,
    integrity = "sha256-No7G08x7VJ+CkjuhyohcTWymPPm0QUE4EZlkp9Of5jM=",
    url = "https://github.com/npaun/bins/releases/download/llvm-18.1.8/llvm-18.1.8-linux-arm64-clang-format",
)

http_file(
    name = "clang-format-linux-amd64",
    executable = True,
    integrity = "sha256-iCbaPg60x60eA9ZIWmSdFva/RD9xOBcJLUwSRK8Gxzk=",
    url = "https://github.com/npaun/bins/releases/download/llvm-18.1.8/llvm-18.1.8-linux-amd64-clang-format",
)

http_file(
    name = "clang-format-windows-amd64",
    executable = True,
    integrity = "sha256-4V2KXVoX5Ny1J7ABfVRx0nAHpAGegykhzac7zW3nK0k=",
    url = "https://github.com/npaun/bins/releases/download/llvm-18.1.8/llvm-18.1.8-windows-amd64-clang-format.exe",
)
