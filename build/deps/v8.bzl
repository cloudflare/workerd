load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")
load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

VERSION = "14.0.365.4"

INTEGRITY = "sha256-rw6N1X1qhhGlhYpUyvkuQ9dDRuoSPXCn0tuYq6uJSbw="

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
    "0011-Revert-heap-Add-masm-specific-unwinding-annotations-.patch",
    "0012-Update-illegal-invocation-error-message-in-v8.patch",
    "0013-Implement-cross-request-context-promise-resolve-hand.patch",
    "0014-Add-another-slot-in-the-isolate-for-embedder.patch",
    "0015-Add-ValueSerializer-SetTreatProxiesAsHostObjects.patch",
    "0016-Disable-memory-leak-assert-when-shutting-down-V8.patch",
    "0017-Enable-V8-shared-linkage.patch",
    "0018-Modify-where-to-look-for-fast_float-and-simdutf.patch",
    "0019-Remove-unneded-latomic-linker-flag.patch",
    "0020-Add-methods-to-get-heap-and-external-memory-sizes-di.patch",
    "0021-Remove-DCHECK-from-WriteOneByteV2-to-skip-v8-fatal.patch",
    "0022-Port-concurrent-mksnapshot-support.patch",
    "0023-Port-V8_USE_ZLIB-support.patch",
    "0024-Modify-where-to-look-for-dragonbox.patch",
    "0025-Disable-slow-handle-check.patch",
    "0026-Workaround-for-builtin-can-allocate-issue.patch",
    "0027-Implement-additional-Exception-construction-methods.patch",
    "0028-Export-icudata-file-to-facilitate-embedding-it.patch",
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
        commit = "1b2e3e8a421efae36141a7b932b41e315b089af8",
        patch_cmds = ["find source -name BUILD.bazel | xargs rm"],
        patch_cmds_win = ["Get-ChildItem -Path source -File -Include BUILD.bazel -Recurse | Remove-Item"],
        remote = "https://chromium.googlesource.com/chromium/deps/icu.git",
    )

    http_archive(
        name = "perfetto",
        integrity = "sha256-T5F4h9xXdYfTGMa+AXmewHIkS1cgxu5ierfyJMOwqJA=",
        strip_prefix = "perfetto-51.2",
        url = "https://github.com/google/perfetto/archive/refs/tags/v51.2.tar.gz",
    )

    # For use with perfetto
    native.new_local_repository(
        name = "perfetto_cfg",
        build_file_content = "",
        path = "build/perfetto",
    )
