# Bazel makes it difficult to define link options so that they can be set to different values for
# tool/test binaries and across different C++ and Rust binaries – define a common configuration for
# linker flags for workerd and downstream builds.

load("@rules_cc//cc:cc_library.bzl", "cc_library")

cc_library(
    name = "linkopts_common",
    # -dead_strip is the macOS equivalent of -ffunction-sections, -Wl,--gc-sections.
    # -no_exported_symbols is used to not include the exports trie, which significantly reduces
    # binary sizes. Unfortunately, the flag and the exports trie are poorly documented. Based
    # on analyzing the binary sections with and without the flag, the information being removed
    # consists of weak binding info, export binding info and stub bindings as described in
    # http://www.newosxbook.com/articles/DYLD.html. The flag itself is described in
    # https://www.wwdcnotes.com/notes/wwdc22/110362/.
    # The affected sections appear to not be needed for debugging and are only used when
    # external code needs to look up bindings in a binary, e.g. when loading a plugin
    # (mac-specific feature). In particular, the symbol table is not affected (the name of the
    # flag is misleading here).
    linkopts = select({
        "@//:use_dead_strip": [
            "-Wl,-dead_strip",
            "-Wl,-no_exported_symbols",
        ],
        "//conditions:default": [""],
    }),
    visibility = ["//visibility:public"],
)

cc_library(
    name = "linkopts_default",
    # For test binaries, reduce thinLTO optimizations and inlining to speed up linking. This
    # only has an effect if thinLTO is enabled.
    # Disabling cross-optimization entirely using -Wl,--lto-O0.
    linkopts = select({
        "@platforms//os:linux": ["-Wl,--lto-O0"],
        "//conditions:default": [""],
    }),
    visibility = ["//visibility:public"],
    # TODO(someday): We previously used turn down thinLTO optimization slightly less – maybe there's
    # a use case for these settings somewhere still?
    # linkopts = ["-Wl,--lto-O1", "-Wl,-mllvm,-import-instr-limit=5"],
    deps = [":linkopts_common"],
)

# link options to use with tools where we don't want to compromise performance, e.g. workerd.
cc_library(
    name = "linkopts_tool",
    # Increase thinLTO parallelism so tool binaries are less of a bottleneck during builds.
    linkopts = select({
        "@platforms//os:linux": ["-Wl,--thinlto-jobs=8"],
        "//conditions:default": [""],
    }),
    visibility = ["//visibility:public"],
    deps = [":linkopts_common"],
)
