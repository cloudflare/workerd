workspace(name = "workerd")

# ========================================================================================
# Bazel basics

load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository", "new_git_repository")
load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive", "http_file")

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

# ========================================================================================
# Simple dependencies

http_archive(
    name = "capnp-cpp",
    sha256 = "a588878af2b2463f6275441d967a4ecc2983addb043c0bbd1ed64149e13c03a6",
    strip_prefix = "capnproto-capnproto-d6f32b3/c++",
    type = "tgz",
    urls = ["https://github.com/capnproto/capnproto/tarball/d6f32b38362749be7fdb7edf28b801ba92d56e7a"],
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
    name = "sqlite3",
    build_file = "//:build/BUILD.sqlite3",
    sha256 = "49112cc7328392aa4e3e5dae0b2f6736d0153430143d21f69327788ff4efe734",
    strip_prefix = "sqlite-amalgamation-3400100",
    type = "zip",
    url = "https://sqlite.org/2022/sqlite-amalgamation-3400100.zip",
    patches = [
        "//:patches/sqlite/0001-row-counts-amalgamation.patch",
    ],
    patch_args = ["-p1"],
)

http_archive(
    name = "rules_python",
    sha256 = "84aec9e21cc56fbc7f1335035a71c850d1b9b5cc6ff497306f84cced9a769841",
    strip_prefix = "rules_python-0.23.1",
    url = "https://github.com/bazelbuild/rules_python/releases/download/0.23.1/rules_python-0.23.1.tar.gz",
)

http_archive(
    name = "com_google_benchmark",
    sha256 = "2aab2980d0376137f969d92848fbb68216abb07633034534fc8c65cc4e7a0e93",
    strip_prefix = "benchmark-1.8.2",
    url = "https://github.com/google/benchmark/archive/refs/tags/v1.8.2.tar.gz",
)

# Using latest brotli commit due to macOS and clang-cl compile issues with v1.0.9, switch to a
# release version later.
http_archive(
    name = "brotli",
    sha256 = "9795b1b2afcc62c254012b1584e849e0c628ceb306756efee8d4539b4c583c09",
    strip_prefix = "google-brotli-ec107cf",
    type = "tgz",
    urls = ["https://github.com/google/brotli/tarball/ec107cf015139c791f79afac0f96c3a2c45e157f"],
)

http_archive(
    name = "ada-url",
    build_file = "//:build/BUILD.ada-url",
    sha256 = "d6be6a559745a79be191bc63c1190015c702a30bacad10028d32b479644a0785",
    type = "zip",
    url = "https://github.com/ada-url/ada/releases/download/v2.7.0/singleheader.zip",
    patches = [],
    patch_args = ["-p1"],
)

# ========================================================================================
# Dawn
#
# WebGPU implementation

git_repository(
    name = "dawn",
    build_file = "//:build/BUILD.dawn",
    commit = "fd61f6244fb00ea42390f5a77267a4c195d90a06",
    patches = [
        "@//:patches/dawn_spirv.patch",
    ],
    remote = "https://dawn.googlesource.com/dawn.git",
)

git_repository(
    name = "vulkan_tools",
    build_file = "//:build/BUILD.vulkan_tools",
    commit = "ca8bb4ee3cc9afdeca4b49c5ef758bad7cce2c72",
    remote = "https://github.com/KhronosGroup/Vulkan-Tools.git",
)

git_repository(
    name = "vulkan_headers",
    build_file = "//:build/BUILD.vulkan_headers",
    commit = "c1a8560c5cf5e7bd6dbc71fe69b1a317411c36b8",
    remote = "https://github.com/KhronosGroup/Vulkan-Headers.git",
)

git_repository(
    name = "spirv_headers",
    commit = "6e09e44cd88a5297433411b2ee52f4cf9f50fa90",
    remote = "https://github.com/KhronosGroup/SPIRV-Headers.git",
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
    sha256 = "3a889795d4dede1094572d98a3a85e9484573a83282a72a6491eae853af07d08",
    strip_prefix = "abseil-abseil-cpp-861e53c",
    type = "tgz",
    url = "https://github.com/abseil/abseil-cpp/tarball/861e53c8f075c8c4d67bd4c82217c57239fc97cf",
)

# tcmalloc requires this "rules_fuzzing" package. Its build files fail analysis without it, even
# though it is unused for our purposes.
http_archive(
    name = "rules_fuzzing",
    sha256 = "bc286c36bf40c5447d8e4ee047f471c934fe99d4acba0de7a866f38d2ea83a21",
    strip_prefix = "rules_fuzzing-0.1.1",
    urls = ["https://github.com/bazelbuild/rules_fuzzing/archive/v0.1.1.tar.gz"],
)

load("@rules_fuzzing//fuzzing:repositories.bzl", "rules_fuzzing_dependencies")

rules_fuzzing_dependencies()

load("@rules_fuzzing//fuzzing:init.bzl", "rules_fuzzing_init")

rules_fuzzing_init()

# OK, now we can bring in tcmalloc itself.
http_archive(
    name = "com_google_tcmalloc",
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

http_file(
    name = "cargo_bazel_linux_x64",
    executable = True,
    sha256 = "802c67ce797673f74f6053206b30c38df5d213cfe576505bd70c3ed85e65687a",
    urls = [
        "https://github.com/bazelbuild/rules_rust/releases/download/0.28.0/cargo-bazel-x86_64-unknown-linux-gnu",
    ],
)

http_file(
    name = "cargo_bazel_linux_arm64",
    executable = True,
    sha256 = "ac65915f702b97479924b290895dd9d759e0883b8a60bce44b9cc43ba4cca18b",
    urls = [
        "https://github.com/bazelbuild/rules_rust/releases/download/0.28.0/cargo-bazel-aarch64-unknown-linux-gnu",
    ],
)

http_file(
    name = "cargo_bazel_macos_x64",
    executable = True,
    sha256 = "756f26c97d46b88fa94f098de0805ae9b6cc25d01708dd7606af57d632bc504a",
    urls = [
        "https://github.com/bazelbuild/rules_rust/releases/download/0.28.0/cargo-bazel-x86_64-apple-darwin",
    ],
)

http_file(
    name = "cargo_bazel_macos_arm64",
    executable = True,
    sha256 = "73ea17706d2b875ecce78015c8534435536211ff52d3ff8e23797457a386bfea",
    urls = [
        "https://github.com/bazelbuild/rules_rust/releases/download/0.28.0/cargo-bazel-aarch64-apple-darwin",
    ],
)

http_file(
    name = "cargo_bazel_win_x64",
    downloaded_file_path = "downloaded.exe",  # .exe extension required for Windows to recognise as executable
    executable = True,
    sha256 = "838f456e84c04b5ba939adcaa017a6bcf87e0d8c9673524463c12c76c314e9a5",
    urls = [
        "https://github.com/bazelbuild/rules_rust/releases/download/0.28.0/cargo-bazel-x86_64-pc-windows-msvc.exe",
    ],
)

http_archive(
    name = "rules_rust",
    sha256 = "c46bdafc582d9bd48a6f97000d05af4829f62d5fee10a2a3edddf2f3d9a232c1",
    urls = [
        "https://github.com/bazelbuild/rules_rust/releases/download/0.28.0/rules_rust-v0.28.0.tar.gz",
    ],
)

load("@rules_rust//rust:repositories.bzl", "rules_rust_dependencies", "rust_register_toolchains")

rules_rust_dependencies()

rust_register_toolchains(
    edition = "2021",
    versions = ["1.69.0"],
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
# It would seem that googlesource would rather we use git protocol (ideally with shallow clones).
# Fine, we can do that.
#
# There is an official mirror for V8 itself on GitHub, but not for dependencies like zlib (Chromium
# fork), icu (Chromium fork), and trace_event, so we still have to use git for them.

http_archive(
    name = "v8",
    sha256 = "5a28ff9feff6d5efee118b11ba994319c63224192f25530e4e296464cf7b8f96",
    patch_args = ["-p1"],
    patches = [
        "//:patches/v8/0001-Allow-manually-setting-ValueDeserializer-format-vers.patch",
        "//:patches/v8/0002-Allow-manually-setting-ValueSerializer-format-versio.patch",
        "//:patches/v8/0003-Make-icudata-target-public.patch",
        "//:patches/v8/0004-Add-ArrayBuffer-MaybeNew.patch",
        "//:patches/v8/0005-Allow-Windows-builds-under-Bazel.patch",
        "//:patches/v8/0006-Disable-bazel-whole-archive-build.patch",
        "//:patches/v8/0007-Make-v8-Locker-automatically-call-isolate-Enter.patch",
        "//:patches/v8/0008-Add-an-API-to-capture-and-restore-the-cage-base-poin.patch",
        "//:patches/v8/0009-Speed-up-V8-bazel-build-by-always-using-target-cfg.patch",
        "//:patches/v8/0010-Implement-Promise-Context-Tagging.patch",
        "//:patches/v8/0011-Enable-V8-shared-linkage.patch",
        "//:patches/v8/0012-Fix-V8-ICU-build.patch",
        "//:patches/v8/0013-Randomize-the-initial-ExecutionContextId-used-by-the.patch",
    ],
    strip_prefix = "v8-v8-934b99f",
    type = "tgz",
    url = "https://github.com/v8/v8/tarball/934b99feafeaaafca2d9e587d586afc8687dea80",
)

new_git_repository(
    name = "com_googlesource_chromium_icu",
    build_file = "@v8//:bazel/BUILD.icu",
    commit = "de4ce0071eb47ed54cbda54869001210cf3a8ae5",
    patch_cmds = ["find source -name BUILD.bazel | xargs rm"],
    patch_cmds_win = ["Get-ChildItem -Path source -File -Include BUILD.bazel -Recurse | Remove-Item"],
    remote = "https://chromium.googlesource.com/chromium/deps/icu.git",
    shallow_since = "1690331196 +0000"
)

new_git_repository(
    name = "com_googlesource_chromium_base_trace_event_common",
    build_file = "@v8//:bazel/BUILD.trace_event_common",
    commit = "147f65333c38ddd1ebf554e89965c243c8ce50b3",
    remote = "https://chromium.googlesource.com/chromium/src/base/trace_event/common.git",
    shallow_since = "1676317690 -0800",
)

# This sets up a hermetic python3, rather than depending on what is installed.
load("@rules_python//python:repositories.bzl", "python_register_toolchains")

python_register_toolchains(
    name = "python3_11",
    ignore_root_user_error = True,
    # https://github.com/bazelbuild/rules_python/blob/main/python/versions.bzl
    python_version = "3.11",
)

load("@python3_11//:defs.bzl", "interpreter")
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
new_local_repository(
    name = "workerd-v8",
    build_file_content = """cc_library(
        name = "v8",
        deps = ["@v8//:v8_icu", "@workerd//:icudata-embed"],
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
        strip_include_prefix = "/",
        visibility = ["//visibility:public"],)""",
    path = "empty",
)
