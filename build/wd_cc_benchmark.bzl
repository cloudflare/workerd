"""wd_cc_benchmark definition"""

def wd_cc_benchmark(
        name,
        linkopts = [],
        deps = [],
        visibility = None,
        **kwargs):
    """Wrapper for cc_binary that sets common attributes and links the benchmark library.
    """

    native.cc_binary(
        name = name,
        defines = ["WD_IS_BENCHMARK"],
        linkopts = linkopts + select({
          "@//:use_dead_strip": ["-Wl,-dead_strip"],
          "//conditions:default": [""],
        }),
        visibility = visibility,
        deps = deps + [
          "@com_google_benchmark//:benchmark_main",
          "//src/workerd/tests:bench-tools"
        ],
        # use the same malloc we use for server
        malloc = "//src/workerd/server:malloc",
        # Only run benchmarks when explicitly requested, at least until we have some more of them
        # and can define a benchmark suite.
        tags = ["manual", "benchmark"],
        **kwargs
    )
