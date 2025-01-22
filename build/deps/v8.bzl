load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")
load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

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
        integrity = "sha256-s+oY+fAG0cXTGmKDxbioUudjUciK1dU2reu7lZ+uB1w=",
        patch_args = ["-p1"],
        patches = [
            "//:patches/v8/0001-Allow-manually-setting-ValueDeserializer-format-vers.patch",
            "//:patches/v8/0002-Allow-manually-setting-ValueSerializer-format-versio.patch",
            "//:patches/v8/0003-Allow-Windows-builds-under-Bazel.patch",
            "//:patches/v8/0004-Disable-bazel-whole-archive-build.patch",
            "//:patches/v8/0005-Speed-up-V8-bazel-build-by-always-using-target-cfg.patch",
            "//:patches/v8/0006-Implement-Promise-Context-Tagging.patch",
            "//:patches/v8/0007-Randomize-the-initial-ExecutionContextId-used-by-the.patch",
            "//:patches/v8/0008-increase-visibility-of-virtual-method.patch",
            "//:patches/v8/0009-Add-ValueSerializer-SetTreatFunctionsAsHostObjects.patch",
            "//:patches/v8/0010-Set-torque-generator-path-to-external-v8.-This-allow.patch",
            "//:patches/v8/0011-Modify-where-to-look-for-fp16-dependency.-This-depen.patch",
            "//:patches/v8/0012-Expose-v8-Symbol-GetDispose.patch",
            "//:patches/v8/0013-Revert-TracedReference-deref-API-removal.patch",
            "//:patches/v8/0014-Revert-heap-Add-masm-specific-unwinding-annotations-.patch",
            "//:patches/v8/0015-Update-illegal-invocation-error-message-in-v8.patch",
            "//:patches/v8/0016-Implement-cross-request-context-promise-resolve-hand.patch",
            "//:patches/v8/0017-Modify-where-to-look-for-fast_float-dependency.patch",
            "//:patches/v8/0018-Return-rejected-promise-from-WebAssembly.compile-if-.patch",
            "//:patches/v8/0019-codegen-Don-t-pass-a-nullptr-in-InitUnwindingRecord-.patch",
            "//:patches/v8/0020-Add-another-slot-in-the-isolate-for-embedder.patch",
            "//:patches/v8/0021-Add-ValueSerializer-SetTreatProxiesAsHostObjects.patch",
            "//:patches/v8/0022-Disable-memory-leak-assert-when-shutting-down-V8.patch",
        ],
        strip_prefix = "v8-13.1.201.8",
        url = "https://github.com/v8/v8/archive/refs/tags/13.1.201.8.tar.gz",
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
        integrity = "sha256-jRxr9E8b2wmKtwzWDaPOa25zHk6yHdUrJSfL3PhdmE0=",
        strip_prefix = "perfetto-48.1",
        url = "https://github.com/google/perfetto/archive/refs/tags/v48.1.tar.gz",
    )

    # For use with perfetto
    native.new_local_repository(
        name = "perfetto_cfg",
        build_file_content = "",
        path = "build/perfetto",
    )
