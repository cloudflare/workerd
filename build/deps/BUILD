# Bazel makes it difficult to define link options so that they can be set to different values for
# tool/test binaries and across different C++ and Rust binaries – define a common configuration for
# linker flags for workerd and downstream builds.

load("@bazel_skylib//rules:common_settings.bzl", "bool_flag")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_python//python:defs.bzl", "py_library")
load("//:build/force_load.bzl", "cc_force_load")
load("//:build/linking.bzl", "MACOS_ALWAYSLINK")

# Whether macOS links use lld, as --config=macos_lld does.
bool_flag(
    name = "macos_lld",
    build_setting_default = False,
    # --config=macos_lld sets the linker of target binaries only, so tools built for the exec
    # configuration still link with Xcode's linker.
    scope = "target",
)

config_setting(
    name = "use_macos_lld",
    flag_values = {":macos_lld": "True"},
)

# -dead_strip is the macOS equivalent of -ffunction-sections, -Wl,--gc-sections.
cc_library(
    name = "linkopts_dead_strip",
    linkopts = select({
        "@//:use_dead_strip": ["-Wl,-dead_strip"],
        "//conditions:default": [""],
    }),
    visibility = ["//visibility:public"],
)

cc_library(
    name = "linkopts_common",
    # -no_exported_symbols is used to not include the exports trie, which significantly reduces
    # binary sizes. Unfortunately, the flag and the exports trie are poorly documented. Based
    # on analyzing the binary sections with and without the flag, the information being removed
    # consists of weak binding info, export binding info and stub bindings as described in
    # http://www.newosxbook.com/articles/DYLD.html. The flag itself is described in
    # https://www.wwdcnotes.com/notes/wwdc22/110362/.
    # The affected sections appear to not be needed for debugging and are only used when
    # external code needs to look up bindings in a binary, e.g. when loading a plugin
    # (mac-specific feature). In particular, the symbol table is not affected (the name of the
    # flag is misleading here). The dylibs that macOS test binaries load look up symbols in the
    # test binary, so linkopts_default leaves this flag out.
    linkopts = select({
        "@//:use_dead_strip": ["-Wl,-no_exported_symbols"],
        "//conditions:default": [""],
    }),
    visibility = ["//visibility:public"],
    deps = [":linkopts_dead_strip"],
)

cc_library(
    name = "linkopts_default",
    # For test binaries, reduce thinLTO optimizations and inlining to speed up linking. This
    # only has an effect if thinLTO is enabled. -Wl,--lto-O0 results in disabling cross-optimization
    # entirely without affecting codegen optimizations within a translation unit. Also limit thinLTO
    # parallelism since our many test binaries may lead to resource exhaustion, but are unlikely to
    # be on the critical path of a build step.
    # TODO(someday): We used to turn down thinLTO optimization slightly less – maybe there's a use
    # case for these settings somewhere still?
    # linkopts = ["-Wl,--lto-O1", "-Wl,-mllvm,-import-instr-limit=5"],
    linkopts = select({
        "@platforms//os:linux": [
            "-Wl,--lto-O0",
            "-Wl,--thinlto-jobs=4",
        ],
        "//conditions:default": [""],
    }) + select({
        # The same with lld on macOS. Xcode clang's ThinLTO bitcode leaves coroutines unsplit for
        # the link, whose -O0 pipeline doesn't split them, so these links run the coroutine passes
        # in its place. coro-split only visits functions that the module's live code reaches, so
        # globaldce first removes the dead ones, which would otherwise reach codegen unsplit. -Wl
        # would split the pass list at its commas. Xcode's linker rejects these options.
        ":use_macos_lld": [
            "-Wl,--lto-O0",
            "-Xlinker",
            "--lto-newpm-passes=globaldce,coro-early,cgscc(coro-split),coro-cleanup",
            "-Wl,--thinlto-jobs=4",
        ],
        "//conditions:default": [],
    }),
    visibility = ["//visibility:public"],
    deps = [":linkopts_dead_strip"] + select({
        "//build/platforms:sanitizer_thread_linux": ["@llvm_runtimes//:runtimes"],
        "//conditions:default": [],
    }),
)

# link options to use with tools where we don't want to compromise performance, e.g. workerd. At
# present, this is identical to linkopts_common, but we may want to move some linker optimizations
# here in the future.
cc_library(
    name = "linkopts_tool",
    visibility = ["//visibility:public"],
    deps = [":linkopts_common"] + select({
        "//build/platforms:sanitizer_thread_linux": ["@llvm_runtimes//:runtimes"],
        "//conditions:default": [],
    }),
)

# zstd, which the dylibs of macOS tests call, but which its module links statically (see
# MACOS_ALWAYSLINK in build/linking.bzl).
cc_force_load(
    name = "zstd",
    dep = "@zstd",
    visibility = ["//visibility:public"],
    alwayslink = MACOS_ALWAYSLINK,
)

# ci/v8_update.py imports this as the top-level module `jsonc`.
py_library(
    name = "jsonc",
    srcs = ["jsonc.py"],
    imports = ["."],
    visibility = ["//ci:__pkg__"],
)
