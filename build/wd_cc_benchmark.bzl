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
        tags = ["benchmark"],
        **kwargs
    )

    # generate benchmark report
    native.genrule(
      name = name + "@benchmark.csv",
      outs = [name + ".benchmark.csv"],
      srcs = [name],
      cmd = "./$(location {}) --benchmark_format=csv > \"$@\"".format(name),
      tags = ["off-by-default", "benchmark_report"],
    )
