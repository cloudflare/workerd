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
    }),
    visibility = ["//visibility:public"],
    deps = [":linkopts_common"],
)

# link options to use with tools where we don't want to compromise performance, e.g. workerd. At
# present, this is identical to linkopts_common, but we may want to move some linker optimizations
# here in the future.
cc_library(
    name = "linkopts_tool",
    visibility = ["//visibility:public"],
    deps = [":linkopts_common"],
)
