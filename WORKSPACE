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
# Simple dependenciess

http_archive(
    name = "capnp-cpp",
    sha256 = "03494ebba861cbe6d141417232f5b306731a1317b81a8ff9830bdd52f60ba456",
    strip_prefix = "capnproto-capnproto-54ce3da/c++",
    type = "tgz",
    urls = ["https://github.com/capnproto/capnproto/tarball/54ce3daa0ff43146bec861ec28747ee15222f032"],
)

http_archive(
    name = "ssl",
    sha256 = "873ec711658f65192e9c58554ce058d1cfa4e57e13ab5366ee16f76d1c757efc",
    strip_prefix = "google-boringssl-ed2e74e",
    type = "tgz",
    # from master-with-bazel branch
    urls = ["https://github.com/google/boringssl/tarball/ed2e74e737dc802ed9baad1af62c1514430a70d6"],
)

http_archive(
    name = "com_cloudflare_lol_html",
    url = "https://github.com/cloudflare/lol-html/tarball/1b64a2ed0d719ce5dfac316108ca2dfad73ff9b4",
    strip_prefix = "cloudflare-lol-html-1b64a2e",
    type = "tgz",
    sha256 = "9648017d74fbb2ab8418efe12e7baff0c0acd4b97f9a0023b562f3a3744b6d7b",
    build_file = "//:build/BUILD.lol-html",
)

# ========================================================================================
# Protobuf
#
# TODO(cleanup): We only even depend on this to build jaeger.proto which isn't really used by
#   workerd -- it's more of an internal dependency that we haven't managed to factor out. If we
#   could eliminate it then maybe we could get rid of this heavy dependency entirely?
#
#   Note that we pick up a definition for `@zlib` from protobuf. We used to pick up the dependency
#   from v8 instead but they removed that dependency (from their bazel build, at least) at commit
#   208bda48. If we remove protobuf then we'll need to define our own version of this dependency,
#   which entales maintaining a BUILD file for it.

http_archive(
    name = "com_google_protobuf",
    url = "https://github.com/protocolbuffers/protobuf/releases/download/v3.19.3/protobuf-cpp-3.19.3.tar.gz",
    strip_prefix = "protobuf-3.19.3",
    type = "tgz",
    sha256 = "f0cc6ae9119d4891d72bc72d44ae73308d6e2319e2ce96c10c22d2508c55c922",
)

load("@com_google_protobuf//:protobuf_deps.bzl", "protobuf_deps")

protobuf_deps()

# ========================================================================================
# Rust bootstrap
#
# workerd uses some Rust libraries, especially lolhtml for implementing HtmlRewriter.

http_file(
    name = "cargo_bazel",
    executable = True,
    sha256 = "a9f81a6fd356fc01e3da2483bdd1f9dfb080b0bdf5a128fa036c048e5b301562",
    urls = [
        "https://github.com/bazelbuild/rules_rust/releases/download/0.10.0/cargo-bazel-x86_64-unknown-linux-gnu",
    ],
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
    version = "1.58.0",
)

load("@rules_rust//crate_universe:repositories.bzl", "crate_universe_dependencies")

crate_universe_dependencies()

load("//rust-deps/crates:crates.bzl", "crate_repositories")

crate_repositories()

load("//rust-deps/cxxbridge_crates:crates.bzl", cxxbridge_repositories = "crate_repositories")

cxxbridge_repositories()

# ========================================================================================
# V8 and its depnedencies
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
    commit = "35952837d5c420b727642b88e28651cb80c3f538",
    shallow_since = "1662649276 +0000",
    patch_args = [ "-p1" ],
    patches = [
        "//:patches/v8/0001-Allow-manually-setting-ValueDeserializer-format-vers.patch",
        "//:patches/v8/0002-Allow-manually-setting-ValueSerializer-format-versio.patch",
        "//:patches/v8/0003-Make-icudata-target-public.patch",
        "//:patches/v8/0004-Add-ArrayBuffer-MaybeNew.patch",
    ],
)

new_git_repository(
    name = "com_googlesource_chromium_icu",
    remote = "https://chromium.googlesource.com/chromium/deps/icu.git",
    commit = "b3070c52557323463e6b9827e2343e60e1b91f85",
    shallow_since = "1660168635 +0000",
    build_file = "@v8//:bazel/BUILD.icu",
    patch_cmds = [ "find source -name BUILD.bazel | xargs rm" ]
)

new_git_repository(
    name = "com_googlesource_chromium_base_trace_event_common",
    remote = "https://chromium.googlesource.com/chromium/src/base/trace_event/common.git",
    commit = "2ba7a48ca6167ee8ef311a7f3bc60b5e5cf5ee79",
    shallow_since = "1659619139 -0700",
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
    name = "zlib_compression_utils",
    actual = "@com_googlesource_chromium_zlib//:zlib_compression_utils",
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
