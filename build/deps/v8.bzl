load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")
load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

VERSION = "13.3.415.18"

INTEGRITY = "sha256-irTC8837o+iRvTVx6YatlscLn3+nJcr5RumtHCMUhW0="

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
    "0010-Set-torque-generator-path-to-external-v8.-This-allow.patch",
    "0011-Modify-where-to-look-for-fp16-dependency.-This-depen.patch",
    "0012-Expose-v8-Symbol-GetDispose.patch",
    "0013-Revert-TracedReference-deref-API-removal.patch",
    "0014-Revert-heap-Add-masm-specific-unwinding-annotations-.patch",
    "0015-Update-illegal-invocation-error-message-in-v8.patch",
    "0016-Implement-cross-request-context-promise-resolve-hand.patch",
    "0017-Modify-where-to-look-for-fast_float-dependency.patch",
    "0018-Add-another-slot-in-the-isolate-for-embedder.patch",
    "0019-Add-ValueSerializer-SetTreatProxiesAsHostObjects.patch",
    "0020-Disable-memory-leak-assert-when-shutting-down-V8.patch",
    "0021-Enable-V8-shared-linkage.patch",
    "0022-Fix-macOS-build.patch",
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
    )

    git_repository(
        name = "com_googlesource_chromium_icu",
        build_file = "@v8//:bazel/BUILD.icu",
        commit = "bbccc2f6efc1b825de5f2c903c48be685cd0cf22",
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
