"""wd_cc_benchmark definition"""

load("@rules_cc//cc:cc_test.bzl", "cc_test")

def wd_cc_benchmark(
        name,
        deps = [],
        tags = [],
        visibility = None,
        **kwargs):
    """Wrapper for cc_binary that sets common attributes and links the benchmark library.
    """
    cc_test(
        name = name,
        defines = ["WD_IS_BENCHMARK"],
        # Use shared linkage for benchmarks, matching the approach used for tests. Unfortunately,
        # bazel does not support shared linkage on macOS and it is broken on Windows, so only
        # enable this on Linux.
        linkstatic = select({
            "@platforms//os:linux": 0,
            "//conditions:default": 1,
        }),
        visibility = visibility,
        deps = deps + [
            "@google_benchmark//:benchmark_main",
            "//src/workerd/tests:bench-tools",
            # Use same linker flags as with test binaries – wd_cc_benchmark is used with
            # microbenchmarks, which will produce relatively accurate results without thinLTO.
            "//build/deps:linkopts_default",
        ],
        # use the same malloc we use for server
        malloc = "//src/workerd/server:malloc",
        tags = ["workerd-benchmark", "google_benchmark"] + tags,
        size = "large",
        **kwargs
    )

    # generate benchmark report
    native.genrule(
        name = name + "@benchmark.csv",
        outs = [name + ".benchmark.csv"],
        srcs = [name],
        cmd = "./$(location {}) --benchmark_format=csv > \"$@\"".format(name),
        tags = ["off-by-default", "benchmark_report", "workerd-benchmark"],
    )
