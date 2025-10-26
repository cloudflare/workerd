"""wd_cc_benchmark definition"""

def wd_cc_benchmark(
        name,
        args = [],
        linkopts = [],
        deps = [],
        visibility = None,
        **kwargs):
    """Wrapper for cc_binary that sets common attributes and links the benchmark library.
    """

    native.cc_binary(
        name = name,
        args = ["--benchmark_min_time=1s"] + args,
        defines = ["WD_IS_BENCHMARK"],
        # Use shared linkage for benchmarks, matching the approach used for tests. Unfortunately,
        # bazel does not support shared linkage on macOS and it is broken on Windows, so only
        # enable this on Linux.
        linkstatic = select({
            "@platforms//os:linux": 0,
            "//conditions:default": 1,
        }),
        linkopts = linkopts + select({
            "@//:use_dead_strip": ["-Wl,-dead_strip", "-Wl,-no_exported_symbols"],
            "//conditions:default": [""],
        }),
        visibility = visibility,
        deps = deps + [
            "@workerd-google-benchmark//:benchmark_main",
            "//src/workerd/tests:bench-tools",
        ],
        # use the same malloc we use for server
        malloc = "//src/workerd/server:malloc",
        tags = ["workerd-benchmark"],
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
