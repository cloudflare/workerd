load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")
load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

VERSION = "13.8.258.19"

INTEGRITY = "sha256-lKz2HkNJQdVv2fvAsgYFsADg+oV6PL1+9KjOotGbU98="

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
    "0011-Seal-the-HandleScope-when-performing-GC.patch",
    "0012-Revert-heap-Add-masm-specific-unwinding-annotations-.patch",
    "0013-Update-illegal-invocation-error-message-in-v8.patch",
    "0014-Implement-cross-request-context-promise-resolve-hand.patch",
    "0015-Add-another-slot-in-the-isolate-for-embedder.patch",
    "0016-Add-ValueSerializer-SetTreatProxiesAsHostObjects.patch",
    "0017-Disable-memory-leak-assert-when-shutting-down-V8.patch",
    "0018-Enable-V8-shared-linkage.patch",
    "0019-Modify-where-to-look-for-fast_float-and-simdutf.patch",
    "0020-Remove-unneded-latomic-linker-flag.patch",
    "0021-Add-methods-to-get-heap-and-external-memory-sizes-di.patch",
    "0022-Remove-DCHECK-from-WriteOneByteV2-to-skip-v8-fatal.patch",
    "0023-Port-concurrent-mksnapshot-support.patch",
    "0024-Port-V8_USE_ZLIB-support.patch",
    "0025-Modify-where-to-look-for-dragonbox.patch",
    "0026-Workaround-for-builtin-can-allocate-issue.patch",
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
        commit = "4c8cc4b365a505ce35be1e0bd488476c5f79805d",
        patch_cmds = ["find source -name BUILD.bazel | xargs rm"],
        patch_cmds_win = ["Get-ChildItem -Path source -File -Include BUILD.bazel -Recurse | Remove-Item"],
        remote = "https://chromium.googlesource.com/chromium/deps/icu.git",
    )

    http_archive(
        name = "perfetto",
        integrity = "sha256-wiMNBHkOtQIxpYYWo/H/bc93LY4iAzOncRYF+Zxcbbk=",
        strip_prefix = "perfetto-50.1",
        url = "https://github.com/google/perfetto/archive/refs/tags/v50.1.tar.gz",
    )

    # For use with perfetto
    native.new_local_repository(
        name = "perfetto_cfg",
        build_file_content = "",
        path = "build/perfetto",
    )
