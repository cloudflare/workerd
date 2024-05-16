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
    sha256 = "c4bb2b7367c484382300aee75be598b92f847896fb31bbd22f3a2346adf66a80",
    url = "https://github.com/bazelbuild/apple_support/releases/download/1.15.1/apple_support.1.15.1.tar.gz",
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
    integrity = "sha256-74Mw9RvA8+zvopkQgG3tvtxFIXMpWjdtVYb32I6uFsw=",
    strip_prefix = "capnproto-capnproto-8653ebc/c++",
    type = "tgz",
    urls = ["https://github.com/capnproto/capnproto/tarball/8653ebc7751d5980e0009bdde80950c2b0935d33"],
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
        "//:patches/sqlite/0002-macOS-missing-PATH-fix.patch",
        "//:patches/sqlite/0003-sqlite-complete-early-exit.patch",
    ],
    sha256 = "ab9aae38a11b931f35d8d1c6d62826d215579892e6ffbf89f20bdce106a9c8c5",
    strip_prefix = "sqlite-src-3440000",
    type = "zip",
    url = "https://sqlite.org/2023/sqlite-src-3440000.zip",
)

http_archive(
    name = "rules_python",
    sha256 = "c68bdc4fbec25de5b5493b8819cfc877c4ea299c0dcb15c244c5a00208cde311",
    strip_prefix = "rules_python-0.31.0",
    url = "https://github.com/bazelbuild/rules_python/releases/download/0.31.0/rules_python-0.31.0.tar.gz",
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
    sha256 = "584faf94049e1e7eaae005e58841092893cd5051a13c8b7cbe7e19510bacc705",
    type = "zip",
    url = "https://github.com/ada-url/ada/releases/download/v2.7.8/singleheader.zip",
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
    sha256 = "c4a4e0c1cb658a39abc0435cc07df902e5a2ecffc091e0528b96b0c295e309ea",
    type = "zip",
    urls = ["https://github.com/dom96/pyodide_packages/releases/download/just-stdlib/pyodide_packages.tar.zip"],
)

load("//:build/pyodide_bucket.bzl", "PYODIDE_LOCK_SHA256", "PYODIDE_GITHUB_RELEASE_URL", "PYODIDE_ALL_WHEELS_ZIP_SHA256")

http_file(
    name = "pyodide-lock.json",
    sha256 = PYODIDE_LOCK_SHA256,
    url = PYODIDE_GITHUB_RELEASE_URL + "pyodide-lock.json",
)

http_archive(
    name = "all_pyodide_wheels",
    sha256 = PYODIDE_ALL_WHEELS_ZIP_SHA256,
    type="zip",
    urls = [PYODIDE_GITHUB_RELEASE_URL + "all_wheels.zip"],
    build_file_content = """
filegroup(
    name = "whls",
    srcs = glob(["*"]),
    visibility = ["//visibility:public"]
)
    """
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
    patches = [
        "//:patches/dawn/0001-Add-iomanip.patch",
        "//:patches/dawn/0002-Add-missing-array-import.patch",
    ],
    patch_args = ["-p1"],
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
    commit = "a64dd87cec79c80c88190265cfea0cbd4027677f",
    remote = "https://chromium.googlesource.com/chromium/src/third_party/abseil-cpp.git",
)

git_repository(
    name = "fp16",
    commit = "0a92994d729ff76a58f692d3028ca1b64b145d91",
    build_file_content = "exports_files(glob([\"**\"]))",
    remote = "https://chromium.googlesource.com/external/github.com/Maratyszcza/FP16.git"
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
    sha256 = "4c69bcf4573888be1f676e1ebeb9e4258b8fe19924a5a8f50a57df5cbe3f6d63",
    strip_prefix = "bazelbuild-rules_fuzzing-1dbcd91",
    type = "tgz",
    url = "https://github.com/bazelbuild/rules_fuzzing/tarball/1dbcd9167300ad226d29972f5f9c925d6d81f441",
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
    sha256 = "22b168aefb066b53e940c25503cfa2a840c635763bc86fae90ed98a641978852",
    strip_prefix = "google-tcmalloc-9b7cd13",
    type = "tgz",
    url = "https://github.com/google/tcmalloc/tarball/9b7cd1327fee6f191ee21b46c014a3a5b17f11ce",
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
    sha256 = "dffd5f2ceb91c5a6c0d0e8df5401160d6a5cf15511416b579c0c3936d22ccb8c",
    urls = [
        "https://github.com/bazelbuild/rules_rust/releases/download/0.42.1/cargo-bazel-x86_64-unknown-linux-gnu",
    ],
)

http_file(
    name = "cargo_bazel_linux_arm64",
    executable = True,
    sha256 = "490b52bd8407613c3aa69b9e3f52635a2fe7631ccb5c5bea9d8d0bc0adfa6d0f",
    urls = [
        "https://github.com/bazelbuild/rules_rust/releases/download/0.42.1/cargo-bazel-aarch64-unknown-linux-gnu",
    ],
)

http_file(
    name = "cargo_bazel_macos_x64",
    executable = True,
    sha256 = "cf873df6f03c94b95af567f5b9a6ff3e1528052cc89cabbee5a330e7c94b75c9",
    urls = [
        "https://github.com/bazelbuild/rules_rust/releases/download/0.42.1/cargo-bazel-x86_64-apple-darwin",
    ],
)

http_file(
    name = "cargo_bazel_macos_arm64",
    executable = True,
    sha256 = "813d490b06e346e94c6cb34f4e9fda10bd85d23dc89711a170eb0fb68a19018c",
    urls = [
        "https://github.com/bazelbuild/rules_rust/releases/download/0.42.1/cargo-bazel-aarch64-apple-darwin",
    ],
)

http_file(
    name = "cargo_bazel_win_x64",
    downloaded_file_path = "downloaded.exe",  # .exe extension required for Windows to recognise as executable
    executable = True,
    sha256 = "dea1f912f7c432cd9f84bd2e7b4ad791e7ccfb0c01a6984ccc6498e0cc8be0a7",
    urls = [
        "https://github.com/bazelbuild/rules_rust/releases/download/0.42.1/cargo-bazel-x86_64-pc-windows-msvc.exe",
    ],
)

# TODO(soon): rules_rust starts producing linker errors on Linux as of 0.39 through 0.42.1, try
# upgrading it again later. This is likely due to https://github.com/bazelbuild/rules_rust/pull/2471.
# The related cargo_bazel package has been updated already - some version mismatch between them is
# generally acceptable.
http_archive(
    name = "rules_rust",
    sha256 = "6501960c3e4da32495d1e1007ded0769a534cb195c30dea36aa54f9d8a3f0361",
    urls = [
        "https://github.com/bazelbuild/rules_rust/releases/download/0.38.0/rules_rust-v0.38.0.tar.gz",
    ],
)

load("@rules_rust//rust:repositories.bzl", "rules_rust_dependencies", "rust_register_toolchains")

rules_rust_dependencies()

rust_register_toolchains(
    edition = "2021",
    versions = ["1.75.0"], # LLVM 17
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
    sha256 = "dddd60acc3f2f30359bef502c9d788f67e33814b0ddd99aa27c5a15eb7a41b8c",
    strip_prefix = "rules_nodejs-6.1.0",
    url = "https://github.com/bazelbuild/rules_nodejs/releases/download/v6.1.0/rules_nodejs-v6.1.0.tar.gz",
)

http_archive(
    name = "aspect_rules_js",
    sha256 = "bfc7ab7895f8f08e950d54a9c7813a58da3f1a0587e53414d72e19b787535c20",
    strip_prefix = "rules_js-1.41.2",
    url = "https://github.com/aspect-build/rules_js/archive/refs/tags/v1.41.2.tar.gz",
)

http_archive(
    name = "aspect_rules_ts",
    sha256 = "c77f0dfa78c407893806491223c1264c289074feefbf706721743a3556fa7cea",
    strip_prefix = "rules_ts-2.2.0",
    url = "https://github.com/aspect-build/rules_ts/archive/refs/tags/v2.2.0.tar.gz",
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
    node_version = "20.12.1",
)

load("@aspect_rules_ts//ts:repositories.bzl", "rules_ts_dependencies", TS_LATEST_VERSION = "LATEST_TYPESCRIPT_VERSION")

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
        "//:patches/v8/0013-increase-visibility-of-virtual-method.patch",
        "//:patches/v8/0014-Add-ValueSerializer-SetTreatFunctionsAsHostObjects.patch",
        "//:patches/v8/0015-Set-torque-generator-path-to-external-v8.-This-allow.patch",
        "//:patches/v8/0016-Modify-where-to-look-for-fp16-dependency.-This-depen.patch",
        "//:patches/v8/0017-Fixup-RunMicrotask-to-restore-async-context-on-termi.patch",
        "//:patches/v8/0018-Expose-v8-Symbol-GetDispose.patch",
        "//:patches/v8/0019-Rename-V8_COMPRESS_POINTERS_IN_ISOLATE_CAGE.patch",
    ],
    integrity = "sha256-61Qzg9XnA/S2Jw1xSDgfGm8VUnrPR483gcsQOqlBNq0=",
    strip_prefix = "v8-12.5.227.3",
    type = "tgz",
    url = "https://github.com/v8/v8/archive/refs/tags/12.5.227.3.tar.gz",
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
    sha256 = "6adf73fd7f90409e479d6ac86529ade2d45f50494c5c10f539226693cb8fe4f7",
    strip_prefix = "protobuf-3.10.1",
    type = "tgz",
    url = "https://github.com/protocolbuffers/protobuf/archive/refs/tags/v3.10.1.tar.gz",
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
