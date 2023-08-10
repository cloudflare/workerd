"""wd_cc_benchmark definition"""

def wd_cc_benchmark(
        name,
        linkopts = [],
        deps = [],
        visibility = None,
        **kwargs):
    """Wrapper for cc_binary that sets common attributes
    """

    # TODO: Play with benchmark perfcounters config setting
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
        **kwargs
    )
