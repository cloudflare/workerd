load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")
load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

VERSION = "13.5.212.10"

INTEGRITY = "sha256-LEez3iJZH0OE91qkYjAeuf251mHuB/c8q5WonQp11a4="

PATCHES = [
    "0001-Allow-manually-setting-ValueDeserializer-format-vers.patch",
    "0002-Allow-manually-setting-ValueSerializer-format-versio.patch",
    "0003-Allow-Windows-builds-under-Bazel.patch",
    "0004-Disable-bazel-whole-archive-build.patch",
    "0005-Speed-up-V8-bazel-build-by-always-using-target-cfg.patch",
    "0006-Implement-Promise-Context-Tagging.patch",
    "0007-Randomize-the-initial-ExecutionContextId-used-by-the.patch",
    "0008-increase-visibility-of-virtual-method.patch",
    "0009-Add-ValueSerializer-SetTreatFunctionsAsHostObjects.patch",
    "0010-Modify-where-to-look-for-fp16-dependency.-This-depen.patch",
    "0011-Expose-v8-Symbol-GetDispose.patch",
    "0012-Revert-TracedReference-deref-API-removal.patch",
    "0013-Revert-heap-Add-masm-specific-unwinding-annotations-.patch",
    "0014-Update-illegal-invocation-error-message-in-v8.patch",
    "0015-Implement-cross-request-context-promise-resolve-hand.patch",
    "0016-Add-another-slot-in-the-isolate-for-embedder.patch",
    "0017-Add-ValueSerializer-SetTreatProxiesAsHostObjects.patch",
    "0018-Disable-memory-leak-assert-when-shutting-down-V8.patch",
    "0019-Enable-V8-shared-linkage.patch",
    "0020-Modify-where-to-look-for-fast_float-and-simdutf.patch",
]

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
def deps_v8():
    http_archive(
        name = "v8",
        integrity = INTEGRITY,
        patch_args = ["-p1"],
        patches = ["//:patches/v8/" + p for p in PATCHES],
        strip_prefix = "v8-" + VERSION,
        url = "https://github.com/v8/v8/archive/refs/tags/" + VERSION + ".tar.gz",
        repo_mapping = {"@abseil-cpp": "@com_google_absl"},
    )

    git_repository(
        name = "com_googlesource_chromium_icu",
        build_file = "@v8//:bazel/BUILD.icu",
        commit = "d30b7b0bb3829f2e220df403ed461a1ede78b774",
        patch_cmds = ["find source -name BUILD.bazel | xargs rm"],
        patch_cmds_win = ["Get-ChildItem -Path source -File -Include BUILD.bazel -Recurse | Remove-Item"],
        remote = "https://chromium.googlesource.com/chromium/deps/icu.git",
    )

    http_archive(
        name = "perfetto",
        integrity = "sha256-CHGpKhYqxWVbfXJPk1mxWnXE6SRycW7bxCJ6ZKRoC+Q=",
        strip_prefix = "perfetto-49.0",
        url = "https://github.com/google/perfetto/archive/refs/tags/v49.0.tar.gz",
    )

    # For use with perfetto
    native.new_local_repository(
        name = "perfetto_cfg",
        build_file_content = "",
        path = "build/perfetto",
    )
