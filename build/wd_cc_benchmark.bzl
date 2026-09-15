"""wd_cc_benchmark definition"""

load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("//:build/linking.bzl", "CC_TEST_LINKSTATIC")

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
        linkstatic = CC_TEST_LINKSTATIC,
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
        # Tag with cpu:4 since this target depends on linkopts_default.
        tags = ["workerd-benchmark", "google_benchmark", "cpu:4"] + tags,
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
