workspace(name = "workerd")

# ========================================================================================
# Bazel basics

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive", "http_file")
load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository", "new_git_repository")

http_archive(
    name = "bazel_skylib",
    sha256 = "f7be3474d42aae265405a592bb7da8e171919d74c16f082a5457840f06054728",
    urls = [
        "https://mirror.bazel.build/github.com/bazelbuild/bazel-skylib/releases/download/1.2.1/bazel-skylib-1.2.1.tar.gz",
        "https://github.com/bazelbuild/bazel-skylib/releases/download/1.2.1/bazel-skylib-1.2.1.tar.gz",
    ],
)

load("@bazel_skylib//:workspace.bzl", "bazel_skylib_workspace")

bazel_skylib_workspace()

http_archive(
    name = "rules_foreign_cc",
    sha256 = "6041f1374ff32ba711564374ad8e007aef77f71561a7ce784123b9b4b88614fc",
    strip_prefix = "rules_foreign_cc-0.8.0",
    url = "https://github.com/bazelbuild/rules_foreign_cc/archive/0.8.0.tar.gz",
)

load("@rules_foreign_cc//foreign_cc:repositories.bzl", "rules_foreign_cc_dependencies")

rules_foreign_cc_dependencies()

# ========================================================================================
# Simple dependencies

http_archive(
    name = "capnp-cpp",
    sha256 = "dfca9b050b0e3b381c39f44a998cbb6885b36ab650bc041b6ade55b11473e0d4",
    strip_prefix = "capnproto-capnproto-6e26d26/c++",
    type = "tgz",
    urls = ["https://github.com/capnproto/capnproto/tarball/6e26d260d1d91e0465ca12bbb5230a1dfa28f00d"],
)

http_archive(
    name = "ssl",
    sha256 = "81bd4b20f53b0aa4bccc3f8bc7c5eda18550a91697ba956668dbeba0e3d0965d",
    strip_prefix = "google-boringssl-f7cf966",
    type = "tgz",
    # from master-with-bazel branch
    urls = ["https://github.com/google/boringssl/tarball/f7cf966f3ddc6923104f6a354bf0ba5c618f3320"],
)

http_archive(
    name = "com_cloudflare_lol_html",
    url = "https://github.com/cloudflare/lol-html/tarball/a0053299f6809c2fa4e3af35a4f64bd8069952ba",
    strip_prefix = "cloudflare-lol-html-a005329",
    type = "tgz",
    sha256 = "eba5f6ce291bc0f8e1ba588573c5e88a6a1ba4264b7961b1a674fdbe334b50c2",
    build_file = "//:build/BUILD.lol-html",
)

http_archive(
    name = "sqlite3",
    url = "https://sqlite.org/2022/sqlite-amalgamation-3400100.zip",
    strip_prefix = "sqlite-amalgamation-3400100",
    type = "zip",
    sha256 = "49112cc7328392aa4e3e5dae0b2f6736d0153430143d21f69327788ff4efe734",
    build_file = "//:build/BUILD.sqlite3",
)

# Using latest brotli commit due to macOS compile issues with v1.0.9, switch to a release version
# later.
# TODO(soon): Using a modified build file as brotli bazel build is broken using clang-cl on
# Windows, make a PR to fix this soon.
http_archive(
    name = "brotli",
    sha256 = "e33f397d86aaa7f3e786bdf01a7b5cff4101cfb20041c04b313b149d34332f64",
    strip_prefix = "google-brotli-ed1995b",
    type = "tgz",
    urls = ["https://github.com/google/brotli/tarball/ed1995b6bda19244070ab5d331111f16f67c8054"],
    build_file = "//:build/BUILD.brotli",
)

# ========================================================================================
# tcmalloc

# tcmalloc requires Abseil.
#
# WARNING: This MUST appear before rules_fuzzing_depnedencies(), below. Otherwise,
#   rules_fuzzing_depnedencies() will choose to pull in a different version of Abseil that is too
#   old for tcmalloc. Absurdly, Bazel simply ignores later attempts to define the same repo name,
#   rather than erroring out. Thus this leads to confusing compiler errors in tcmalloc complaining
#   that ABSL_ATTRIBUTE_PURE_FUNCTION is not defined.
http_archive(
    name = "com_google_absl",
    urls = ["https://github.com/abseil/abseil-cpp/archive/b3162b1da62711c663d0025e2eabeb83fd1f2728.zip"],
    strip_prefix = "abseil-cpp-b3162b1da62711c663d0025e2eabeb83fd1f2728",
    sha256 = "d5c91248c33269fcc7ab35897315a45cfa2c37abb4c6d4ed36cb5c82f366367a",
)

# tcmalloc requires this "rules_fuzzing" package. Its build files fail analysis without it, even
# though it is unused for our purposes.
http_archive(
    name = "rules_fuzzing",
    sha256 = "a5734cb42b1b69395c57e0bbd32ade394d5c3d6afbfe782b24816a96da24660d",
    strip_prefix = "rules_fuzzing-0.1.1",
    urls = ["https://github.com/bazelbuild/rules_fuzzing/archive/v0.1.1.zip"],
)

load("@rules_fuzzing//fuzzing:repositories.bzl", "rules_fuzzing_dependencies")

rules_fuzzing_dependencies()

load("@rules_fuzzing//fuzzing:init.bzl", "rules_fuzzing_init")

rules_fuzzing_init()

# OK, now we can bring in tcmalloc itself.
http_archive(
    name = "com_google_tcmalloc",
    url = "https://github.com/google/tcmalloc/tarball/ca82471188f4832e82d2e77078ecad66f4c425d5",
    strip_prefix = "google-tcmalloc-ca82471",
    type = "tgz",
    sha256 = "10b1217154c2b432241ded580d6b0e0b01f5d2566b4eeacf2edf937b87683274",
)

# ========================================================================================
# Rust bootstrap
#
# workerd uses some Rust libraries, especially lolhtml for implementing HtmlRewriter.

http_file(
    name = "cargo_bazel_linux_x64",
    executable = True,
    sha256 = "a9f81a6fd356fc01e3da2483bdd1f9dfb080b0bdf5a128fa036c048e5b301562",
    urls = [
        "https://github.com/bazelbuild/rules_rust/releases/download/0.10.0/cargo-bazel-x86_64-unknown-linux-gnu",
    ],
)

http_file(
    name = "cargo_bazel_linux_arm64",
    executable = True,
    sha256 = "f2d168c386d38c0d5ca429c34dcbc5a6aec5be19ee1d4f6f0e614293b0e55468",
    urls = [
        "https://github.com/bazelbuild/rules_rust/releases/download/0.10.0/cargo-bazel-aarch64-unknown-linux-gnu",
    ],
)

http_file(
    name = "cargo_bazel_macos_x64",
    executable = True,
    sha256 = "fb80acb9fcfd83674f73e98bf956bc65b33f31a4380ba72fbc1a6a9bf22c2f8c",
    urls = [
        "https://github.com/bazelbuild/rules_rust/releases/download/0.10.0/cargo-bazel-x86_64-apple-darwin",
    ],
)

http_file(
    name = "cargo_bazel_macos_arm64",
    executable = True,
    sha256 = "4104ea8edd3fccbcfc43265e4fa02dfc25b12b32250ff46456b829ab9cb78908",
    urls = [
        "https://github.com/bazelbuild/rules_rust/releases/download/0.10.0/cargo-bazel-aarch64-apple-darwin",
    ],
)

http_file(
    name = "cargo_bazel_win_x64",
    executable = True,
    sha256 = "a57c496e8ff9d1b2dcd4f6a3a43c41ed0c54e9f3d48183ed411097c3590176d3",
    urls = [
        "https://github.com/bazelbuild/rules_rust/releases/download/0.10.0/cargo-bazel-x86_64-pc-windows-msvc.exe",
    ],
    downloaded_file_path = "downloaded.exe" # .exe extension required for Windows to recognise as executable
)

http_archive(
    name = "rules_rust",
    sha256 = "0cc7e6b39e492710b819e00d48f2210ae626b717a3ab96e048c43ab57e61d204",
    urls = [
        "https://github.com/bazelbuild/rules_rust/releases/download/0.10.0/rules_rust-v0.10.0.tar.gz",
    ],
)

load("@rules_rust//rust:repositories.bzl", "rules_rust_dependencies", "rust_register_toolchains")

rules_rust_dependencies()

rust_register_toolchains(
    edition = "2018",
    version = "1.66.0",
)

load("@rules_rust//crate_universe:repositories.bzl", "crate_universe_dependencies")

crate_universe_dependencies()

load("//rust-deps/crates:crates.bzl", "crate_repositories")

crate_repositories()

load("//rust-deps/cxxbridge_crates:crates.bzl", cxxbridge_repositories = "crate_repositories")

cxxbridge_repositories()

load("@rules_rust//tools/rust_analyzer:deps.bzl", "rust_analyzer_dependencies")

rust_analyzer_dependencies()

# ========================================================================================
# Node.js bootstrap
#
# workerd uses Node.js scripts for generating TypeScript types.

http_archive(
    name = "aspect_rules_js",
    sha256 = "686d5b345592c1958b4aea24049d935ada11b83ae5538658d22b84b353cfbb1e",
    strip_prefix = "rules_js-1.13.1",
    url = "https://github.com/aspect-build/rules_js/archive/refs/tags/v1.13.1.tar.gz",
)

http_archive(
    name = "aspect_rules_ts",
    sha256 = "6406905c5f7c5ca6dedcca5dacbffbf32bb2a5deb77f50da73e7195b2b7e8cbc",
    strip_prefix = "rules_ts-1.0.5",
    url = "https://github.com/aspect-build/rules_ts/archive/refs/tags/v1.0.5.tar.gz",
)

load("@aspect_rules_js//js:repositories.bzl", "rules_js_dependencies")

rules_js_dependencies()

load("@rules_nodejs//nodejs:repositories.bzl", "nodejs_register_toolchains")

nodejs_register_toolchains(
    name = "nodejs",
    node_version = "18.10.0",
)

load("@aspect_rules_ts//ts:repositories.bzl", TS_LATEST_VERSION = "LATEST_VERSION", "rules_ts_dependencies")

rules_ts_dependencies(ts_version = TS_LATEST_VERSION)

load("@aspect_rules_js//npm:npm_import.bzl", "npm_translate_lock")

npm_translate_lock(
    name = "npm",
    pnpm_lock = "//:pnpm-lock.yaml",
    # Patches required for `capnp-ts` to type-check
    patches = {
        "capnp-ts@0.7.0": ["//:patches/capnp-ts@0.7.0.patch"],
    },
    patch_args = {
        "capnp-ts@0.7.0": ["-p1"],
    },
)

load("@npm//:repositories.bzl", "npm_repositories")

npm_repositories()

# ========================================================================================
# V8 and its dependencies
#
# Note that googlesource does not generate tarballs deterministically, so we cannot use
# http_archive: https://github.com/google/gitiles/issues/84
#
# It would seem that googlesource would rather we use git protocol (ideally with shallow clones).
# Fine, we can do that.
#
# Note that while there is an official mirror of V8 on GitHub, there do not appear to be
# mirrors of the dependencies: zlib (Chromium fork), icu (Chromium fork), and trace_event. So
# fetching from GitHub instead wouldn't really solve the problem.

git_repository(
    name = "v8",
    remote = "https://chromium.googlesource.com/v8/v8.git",
    commit = "84b0aff45ebe07dce522d2c90b42074b25b60927",
    shallow_since = "1685722300 +0000",
    patch_args = [ "-p1" ],
    patches = [
        "//:patches/v8/0001-Allow-manually-setting-ValueDeserializer-format-vers.patch",
        "//:patches/v8/0002-Allow-manually-setting-ValueSerializer-format-versio.patch",
        "//:patches/v8/0003-Make-icudata-target-public.patch",
        "//:patches/v8/0004-Add-ArrayBuffer-MaybeNew.patch",
        "//:patches/v8/0005-Allow-compiling-on-macOS-catalina-and-ventura.patch",
        "//:patches/v8/0006-Fix-v8-code_generator-imports.patch",
        "//:patches/v8/0007-Allow-Windows-builds-under-Bazel.patch",
        "//:patches/v8/0008-Disable-bazel-whole-archive-build.patch",
        "//:patches/v8/0009-Make-v8-Locker-automatically-call-isolate-Enter.patch",
        "//:patches/v8/0010-Add-an-API-to-capture-and-restore-the-cage-base-poin.patch",
    ],
)

new_git_repository(
    name = "com_googlesource_chromium_icu",
    remote = "https://chromium.googlesource.com/chromium/deps/icu.git",
    commit = "a2961dc659b4ae847a9c6120718cc2517ee57d9e",
    shallow_since = "1683080067 +0000",
    build_file = "@v8//:bazel/BUILD.icu",
    patch_cmds = [ "find source -name BUILD.bazel | xargs rm" ],
    patch_cmds_win = [ "Get-ChildItem -Path source -File -Include BUILD.bazel -Recurse | Remove-Item" ],
)

new_git_repository(
    name = "com_googlesource_chromium_base_trace_event_common",
    remote = "https://chromium.googlesource.com/chromium/src/base/trace_event/common.git",
    commit = "147f65333c38ddd1ebf554e89965c243c8ce50b3",
    shallow_since = "1676317690 -0800",
    build_file = "@v8//:bazel/BUILD.trace_event_common",
)

http_archive(
    name = "rules_python",
    sha256 = "a30abdfc7126d497a7698c29c46ea9901c6392d6ed315171a6df5ce433aa4502",
    strip_prefix = "rules_python-0.6.0",
    url = "https://github.com/bazelbuild/rules_python/archive/0.6.0.tar.gz",
)

load("@rules_python//python:pip.bzl", "pip_install")

pip_install(
    name = "v8_python_deps",
    extra_pip_args = ["--require-hashes"],
    requirements = "@v8//:bazel/requirements.txt",
)

bind(
    name = "icu",
    actual = "@com_googlesource_chromium_icu//:icu",
)

bind(
    name = "base_trace_event_common",
    actual = "@com_googlesource_chromium_base_trace_event_common//:trace_event_common",
)

# Tell workerd code where to find v8.
#
# We indirect through `@workerd-v8` to allow dependents to override how and where `v8` is built.
#
# TODO(cleanup): There must be a better way to do this?
new_local_repository(
    name = "workerd-v8",
    build_file_content = """cc_library(
        name = "v8",
        deps = ["@v8//:v8_icu", "@workerd//:icudata-embed"],
        visibility = ["//visibility:public"])""",
    path = "empty",
)

# ========================================================================================
# Tools

# Hedron's Compile Commands Extractor for Bazel
# https://github.com/hedronvision/bazel-compile-commands-extractor
http_archive(
    name = "hedron_compile_commands",
    sha256 = "ab6c6b4ceaf12b224e571ec075fd79086c52c3430993140bb2ed585b08dfc552",
    strip_prefix = "bazel-compile-commands-extractor-d1e95ec162e050b04d0a191826f9bc478de639f7",
    url = "https://github.com/hedronvision/bazel-compile-commands-extractor/archive/d1e95ec162e050b04d0a191826f9bc478de639f7.tar.gz",
)

load("@hedron_compile_commands//:workspace_setup.bzl", "hedron_compile_commands_setup")

hedron_compile_commands_setup()
