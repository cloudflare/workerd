workspace(name = "workerd")

# ========================================================================================
# Bazel basics

load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")
load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive", "http_file")

http_archive(
    name = "bazel_skylib",
    sha256 = "cd55a062e763b9349921f0f5db8c3933288dc8ba4f76dd9416aac68acee3cb94",
    urls = [
        "https://mirror.bazel.build/github.com/bazelbuild/bazel-skylib/releases/download/1.5.0/bazel-skylib-1.5.0.tar.gz",
        "https://github.com/bazelbuild/bazel-skylib/releases/download/1.5.0/bazel-skylib-1.5.0.tar.gz",
    ],
)

load("@bazel_skylib//:workspace.bzl", "bazel_skylib_workspace")

bazel_skylib_workspace()

# Needed for objc_library starting with bazel 7.

http_archive(
    name = "build_bazel_apple_support",
    sha256 = "cf4d63f39c7ba9059f70e995bf5fe1019267d3f77379c2028561a5d7645ef67c",
    url = "https://github.com/bazelbuild/apple_support/releases/download/1.11.1/apple_support.1.11.1.tar.gz",
)

load(
    "@build_bazel_apple_support//lib:repositories.bzl",
    "apple_support_dependencies",
)

apple_support_dependencies()

# ========================================================================================
# Simple dependencies

http_archive(
    name = "capnp-cpp",
    sha256 = "bed3c01caad985c91b58c8123cedfe46c4135d1698303d89c89bc49d3deb3697",
    strip_prefix = "capnproto-capnproto-6480280/c++",
    type = "tgz",
    urls = ["https://github.com/capnproto/capnproto/tarball/64802802df1d7780625eeb07b71d249fe49fb68d"],
)

http_archive(
    name = "ssl",
    sha256 = "57261442e663ad0a0dc5c4eae59322440bfce61f1edc4fe4338179a6abc14034",
    strip_prefix = "google-boringssl-8ae84b5",
    type = "tgz",
    # from master-with-bazel branch
    urls = ["https://github.com/google/boringssl/tarball/8ae84b558b3d3af50a323c7e3800998764e77375"],
)

http_archive(
    name = "sqlite3",
    build_file = "//:build/BUILD.sqlite3",
    patch_args = ["-p1"],
    patches = [
        "//:patches/sqlite/0001-row-counts-plain.patch",
    ],
    sha256 = "ab9aae38a11b931f35d8d1c6d62826d215579892e6ffbf89f20bdce106a9c8c5",
    strip_prefix = "sqlite-src-3440000",
    type = "zip",
    url = "https://sqlite.org/2023/sqlite-src-3440000.zip",
)

http_archive(
    name = "rules_python",
    sha256 = "9d04041ac92a0985e344235f5d946f71ac543f1b1565f2cdbc9a2aaee8adf55b",
    strip_prefix = "rules_python-0.26.0",
    url = "https://github.com/bazelbuild/rules_python/releases/download/0.26.0/rules_python-0.26.0.tar.gz",
)

load("@rules_python//python:repositories.bzl", "py_repositories", "python_register_toolchains")

py_repositories()

http_archive(
    name = "com_google_benchmark",
    sha256 = "2aab2980d0376137f969d92848fbb68216abb07633034534fc8c65cc4e7a0e93",
    strip_prefix = "benchmark-1.8.2",
    url = "https://github.com/google/benchmark/archive/refs/tags/v1.8.2.tar.gz",
)

# These are part of what's needed to get `bazel query 'deps(//...)'`, to work, but this is difficult to support
# based on our dependencies – just use cquery instead.
# load("@com_google_benchmark//:bazel/benchmark_deps.bzl", "benchmark_deps")
# benchmark_deps()

http_archive(
    name = "brotli",
    sha256 = "e720a6ca29428b803f4ad165371771f5398faba397edf6778837a18599ea13ff",
    strip_prefix = "brotli-1.1.0",
    type = "tgz",
    urls = ["https://github.com/google/brotli/archive/refs/tags/v1.1.0.tar.gz"],
)

http_archive(
    name = "ada-url",
    build_file = "//:build/BUILD.ada-url",
    patch_args = ["-p1"],
    patches = [],
    sha256 = "d6be6a559745a79be191bc63c1190015c702a30bacad10028d32b479644a0785",
    type = "zip",
    url = "https://github.com/ada-url/ada/releases/download/v2.7.0/singleheader.zip",
)

http_archive(
    name = "pyodide",
    build_file = "//:build/BUILD.pyodide",
    sha256 = "fbda450a64093a8d246c872bb901ee172a57fe594c9f35bba61f36807c73300d",
    type = "tar.bz2",
    urls = ["https://github.com/pyodide/pyodide/releases/download/0.26.0a2/pyodide-core-0.26.0a2.tar.bz2"],
)

http_archive(
    name = "pyodide_packages",
    build_file = "//:build/BUILD.pyodide_packages",
    sha256 = "f9aa1e044567f1f3e36c3516d066481093dbc116032c45294eee400628d8b4a2",
    type = "zip",
    urls = ["https://github.com/dom96/pyodide_packages/releases/download/just-micropip/pyodide_packages.tar.zip"],
)

pyodide_package_bucket_url = "https://pub-45d734c4145d4285b343833ee450ef38.r2.dev/v1/"

http_file(
    name = "pyodide-lock.json",
    url = pyodide_package_bucket_url + "pyodide-lock.json",
    sha256 = "0678eda79c16f9f7271743ecbe531e59ca2e40d2d4e0dfd3f78191302b989520",
)

# ========================================================================================
# Dawn
#
# WebGPU implementation

git_repository(
    name = "dawn",
    build_file = "//:build/BUILD.dawn",
    commit = "c5169ef5b9982e17a8caddd1218aa0ad5e24a4e3",
    remote = "https://dawn.googlesource.com/dawn.git",
    repo_mapping = {
        "@abseil_cpp": "@com_google_absl",
    },
)

http_archive(
    name = "vulkan_utility_libraries",
    build_file = "//:build/BUILD.vulkan_utility_libraries",
    sha256 = "11a51175598c84ba171fd82ba7f1a109ee4133338684d84f6b3c4bbe9ea52a8d",
    strip_prefix = "KhronosGroup-Vulkan-Utility-Libraries-5b3147a",
    type = "tgz",
    url = "https://github.com/KhronosGroup/Vulkan-Utility-Libraries/tarball/5b3147a535e28a48ae760efacdf97b296d9e8c73",
)

http_archive(
    name = "vulkan_headers",
    build_file = "//:build/BUILD.vulkan_headers",
    sha256 = "559d4bff13acddb58e08bdd862aa6f7fccfda9a97d1799a7f8592e847c723a03",
    strip_prefix = "KhronosGroup-Vulkan-Headers-aff5071",
    type = "tgz",
    url = "https://github.com/KhronosGroup/Vulkan-Headers/tarball/aff5071d4ee6215c60a91d8d983cad91bb25fb57",
)

http_archive(
    name = "spirv_headers",
    sha256 = "c1ef22607cc34489933d987f55b59ad5b3ef98b1f22fc16b2b603de23950aca6",
    strip_prefix = "KhronosGroup-SPIRV-Headers-88bc5e3",
    type = "tgz",
    url = "https://github.com/KhronosGroup/SPIRV-Headers/tarball/88bc5e321c2839707df8b1ab534e243e00744177",
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
    commit = "0764ad493e54a79c7e3e02fc3412ef55b4835b9e",
    remote = "https://chromium.googlesource.com/chromium/src/third_party/abseil-cpp.git",
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
    sha256 = "f6f3f42c48576acd5653bf07637deee2ae4ebb77ccdb0dacc67c184508bedc8c",
    strip_prefix = "rules_fuzzing-0.4.1",
    urls = ["https://github.com/bazelbuild/rules_fuzzing/archive/v0.4.1.tar.gz"],
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
    patch_args = ["-p1"],
    patches = [
        "//:patches/tcmalloc/0001-Replace-ANNOTATE_MEMORY_IS_INITIALIZED-with-ABSL_ANN.patch",
    ],
    sha256 = "10b1217154c2b432241ded580d6b0e0b01f5d2566b4eeacf2edf937b87683274",
    strip_prefix = "google-tcmalloc-ca82471",
    type = "tgz",
    url = "https://github.com/google/tcmalloc/tarball/ca82471188f4832e82d2e77078ecad66f4c425d5",
)

# ========================================================================================
# Rust bootstrap
#
# workerd uses some Rust libraries, especially lolhtml for implementing HtmlRewriter.
# Note that lol_html itself is not included here to avoid dependency duplication and simplify
# the build process. To update the dependency, update the reference commit in
# rust-deps/BUILD.bazel and run `bazel run //rust-deps:crates_vendor -- --repin`

# Based on https://github.com/bazelbuild/bazel/blob/master/third_party/zlib/BUILD.
_zlib_build = """
cc_library(
    name = "zlib",
    srcs = glob(["*.c"]),
    hdrs = glob(["*.h"]),
    includes = ["."],
    # Workaround for zlib warnings and mac compilation. Some issues were resolved in v1.3, but there are still implicit function declarations.
    copts = [
        "-w",
        "-Dverbose=-1",
    ] + select({
        "@platforms//os:linux": [ "-Wno-implicit-function-declaration" ],
        "@platforms//os:macos": [ "-Wno-implicit-function-declaration" ],
        "//conditions:default": [],
    }),
    visibility = ["//visibility:public"],
)
"""

http_archive(
    name = "zlib",
    build_file_content = _zlib_build,
    sha256 = "38ef96b8dfe510d42707d9c781877914792541133e1870841463bfa73f883e32",
    strip_prefix = "zlib-1.3.1",
    # Using the .tar.xz artifact from the release page – for many other dependencies we use a
    # snapshot based on the tag of a release instead.
    urls = ["https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.xz"],
)

http_file(
    name = "cargo_bazel_linux_x64",
    executable = True,
    sha256 = "cd19f960cb97b1ee7f31c9297f8e6f7f08a229e28ab4bb1c2c776a7aba2e211d",
    urls = [
        "https://github.com/bazelbuild/rules_rust/releases/download/0.32.0/cargo-bazel-x86_64-unknown-linux-gnu",
    ],
)

http_file(
    name = "cargo_bazel_linux_arm64",
    executable = True,
    sha256 = "0d9c9b089737b3d3dea5cc5ce2c42ea5cbcfd0e103c47a00ab29953d65dc0b2d",
    urls = [
        "https://github.com/bazelbuild/rules_rust/releases/download/0.32.0/cargo-bazel-aarch64-unknown-linux-gnu",
    ],
)

http_file(
    name = "cargo_bazel_macos_x64",
    executable = True,
    sha256 = "b3f02c5691ceeac06869ce1a7aff06094879b51941fadd57393a55ee0598448f",
    urls = [
        "https://github.com/bazelbuild/rules_rust/releases/download/0.32.0/cargo-bazel-x86_64-apple-darwin",
    ],
)

http_file(
    name = "cargo_bazel_macos_arm64",
    executable = True,
    sha256 = "f9968243c677349a8cbbea360e39e3f9bb696cf853c031ece57e887ecd1bf523",
    urls = [
        "https://github.com/bazelbuild/rules_rust/releases/download/0.32.0/cargo-bazel-aarch64-apple-darwin",
    ],
)

http_file(
    name = "cargo_bazel_win_x64",
    downloaded_file_path = "downloaded.exe",  # .exe extension required for Windows to recognise as executable
    executable = True,
    sha256 = "6da386d85533ce38d7501a41bd072fb5cd27c7f9d801d2336150eeb9f8cf3849",
    urls = [
        "https://github.com/bazelbuild/rules_rust/releases/download/0.32.0/cargo-bazel-x86_64-pc-windows-msvc.exe",
    ],
)

http_archive(
    name = "rules_rust",
    sha256 = "1e7114ea2af800c6987ca38daeee13e3ae6e934875b4f7ca24b798857f95431e",
    urls = [
        "https://github.com/bazelbuild/rules_rust/releases/download/0.32.0/rules_rust-v0.32.0.tar.gz",
    ],
)

load("@rules_rust//rust:repositories.bzl", "rules_rust_dependencies", "rust_register_toolchains")

rules_rust_dependencies()

rust_register_toolchains(
    edition = "2021",
    versions = ["1.72.1"],
)

load("@rules_rust//crate_universe:repositories.bzl", "crate_universe_dependencies")

crate_universe_dependencies()

load("//rust-deps/crates:crates.bzl", "crate_repositories")

crate_repositories()

load("@rules_rust//tools/rust_analyzer:deps.bzl", "rust_analyzer_dependencies")

rust_analyzer_dependencies()

# ========================================================================================
# Node.js bootstrap
#
# workerd uses Node.js scripts for generating TypeScript types.

# Fetch rules_nodejs before aspect_rules_js, otherwise we'll get an outdated rules_nodejs version.
http_archive(
    name = "rules_nodejs",
    sha256 = "162f4adfd719ba42b8a6f16030a20f434dc110c65dc608660ef7b3411c9873f9",
    strip_prefix = "rules_nodejs-6.0.2",
    url = "https://github.com/bazelbuild/rules_nodejs/releases/download/v6.0.2/rules_nodejs-v6.0.2.tar.gz",
)

http_archive(
    name = "aspect_rules_js",
    sha256 = "72e8b34ed850a5acc39b4c85a8d5a0a5063e519e4688200ee41076bb0c979207",
    strip_prefix = "rules_js-1.33.1",
    url = "https://github.com/aspect-build/rules_js/archive/refs/tags/v1.33.1.tar.gz",
)

http_archive(
    name = "aspect_rules_ts",
    sha256 = "4c3f34fff9f96ffc9c26635d8235a32a23a6797324486c7d23c1dfa477e8b451",
    strip_prefix = "rules_ts-1.4.5",
    url = "https://github.com/aspect-build/rules_ts/archive/refs/tags/v1.4.5.tar.gz",
)

load("@aspect_rules_js//js:repositories.bzl", "rules_js_dependencies")

rules_js_dependencies()

load("@rules_nodejs//nodejs:repositories.bzl", "nodejs_register_toolchains")

nodejs_register_toolchains(
    name = "nodejs",
    node_urls = [
        # github workflows may substitute a mirror URL here to avoid fetch failures.
        # "WORKERS_MIRROR_URL/https://nodejs.org/dist/v{version}/{filename}",
        "https://nodejs.org/dist/v{version}/{filename}",
    ],
    node_version = "20.8.0",
)

load("@aspect_rules_ts//ts:repositories.bzl", "rules_ts_dependencies", TS_LATEST_VERSION = "LATEST_VERSION")

rules_ts_dependencies(ts_version = TS_LATEST_VERSION)

load("@aspect_rules_js//npm:npm_import.bzl", "npm_translate_lock")

npm_translate_lock(
    name = "npm",
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
    patch_args = ["-p1"],
    patches = [
        "//:patches/v8/0001-Allow-manually-setting-ValueDeserializer-format-vers.patch",
        "//:patches/v8/0002-Allow-manually-setting-ValueSerializer-format-versio.patch",
        "//:patches/v8/0003-Add-ArrayBuffer-MaybeNew.patch",
        "//:patches/v8/0004-Allow-Windows-builds-under-Bazel.patch",
        "//:patches/v8/0005-Disable-bazel-whole-archive-build.patch",
        "//:patches/v8/0006-Make-v8-Locker-automatically-call-isolate-Enter.patch",
        "//:patches/v8/0007-Add-an-API-to-capture-and-restore-the-cage-base-poin.patch",
        "//:patches/v8/0008-Speed-up-V8-bazel-build-by-always-using-target-cfg.patch",
        "//:patches/v8/0009-Implement-Promise-Context-Tagging.patch",
        "//:patches/v8/0010-Enable-V8-shared-linkage.patch",
        "//:patches/v8/0011-Randomize-the-initial-ExecutionContextId-used-by-the.patch",
        "//:patches/v8/0012-Always-enable-continuation-preserved-data-in-the-bui.patch",
        "//:patches/v8/0013-Attach-continuation-context-to-Promise-thenable-task.patch",
        "//:patches/v8/0014-increase-visibility-of-virtual-method.patch",
    ],
    integrity = "sha256-jcBk1hBhzrMHRL0EDTgHKBVrJPsP1SLZL6A5/l6arrs=",
    strip_prefix = "v8-12.2.281.18",
    type = "tgz",
    url = "https://github.com/v8/v8/archive/refs/tags/12.2.281.18.tar.gz",
)

git_repository(
    name = "com_googlesource_chromium_icu",
    build_file = "@v8//:bazel/BUILD.icu",
    commit = "a622de35ac311c5ad390a7af80724634e5dc61ed",
    patch_cmds = ["find source -name BUILD.bazel | xargs rm"],
    patch_cmds_win = ["Get-ChildItem -Path source -File -Include BUILD.bazel -Recurse | Remove-Item"],
    remote = "https://chromium.googlesource.com/chromium/deps/icu.git",
    shallow_since = "1697047535 +0000",
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
    type = "tgz",
    url = "https://github.com/google/perfetto/archive/refs/tags/v39.0.tar.gz",
)

# For use with perfetto
http_archive(
    name = "com_google_protobuf",
    sha256 = "2ee9dcec820352671eb83e081295ba43f7a4157181dad549024d7070d079cf65",
    strip_prefix = "protobuf-3.9.0",
    type = "tgz",
    url = "https://github.com/protocolbuffers/protobuf/archive/refs/tags/v3.9.0.tar.gz",
)

# For use with perfetto
new_local_repository(
    name = "perfetto_cfg",
    build_file_content = "",
    path = "build/perfetto",
)

git_repository(
    name = "com_googlesource_chromium_base_trace_event_common",
    build_file = "@v8//:bazel/BUILD.trace_event_common",
    commit = "29ac73db520575590c3aceb0a6f1f58dda8934f6",
    remote = "https://chromium.googlesource.com/chromium/src/base/trace_event/common.git",
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
    requirements = "@v8//:bazel/requirements.txt",
)

load("@v8_python_deps//:requirements.bzl", v8_python_deps_install = "install_deps")

v8_python_deps_install()

pip_parse(
    name = "py_deps",
    python_interpreter_target = interpreter,
    requirements = "//build/deps:requirements.txt",
)

load("@py_deps//:requirements.bzl", py_deps_install = "install_deps")

py_deps_install()

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

# rust-based lolhtml dependency, including the API header. See rust-deps for details.
new_local_repository(
    name = "com_cloudflare_lol_html",
    build_file_content = """cc_library(
        name = "lolhtml",
        hdrs = ["@workerd//rust-deps:lol_html_api"],
        deps = ["@workerd//rust-deps"],
        # TODO(soon): This workaround appears to be needed when linking the rust library - figure
        # out why and develop a better approach to address this.
        linkopts = select({
          "@platforms//os:windows": ["ntdll.lib"],
          "//conditions:default": [""],
        }),
        include_prefix = "c-api/include",
        strip_include_prefix = "c-api/include",
        visibility = ["//visibility:public"],)""",
    path = "empty",
)
