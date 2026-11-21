"""wd_cc_benchmark definition"""

load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("//:build/linking.bzl", "CC_TEST_LINKSTATIC")

def wd_cc_benchmark(
        name,
        deps = [],
        tags = [],
        visibility = None,
        tool = False,
        **kwargs):
    """Wrapper for cc_binary that sets common attributes and links the benchmark library.

    Args:
      name: The benchmark target's name.
      deps: Libraries the benchmark uses.
      tags: Tags added to the benchmark's own.
      visibility: The target's visibility.
      tool: True to link like wd_cc_binary: statically, with linkopts_tool. False to link like
        a test binary (dynamically on Linux, with linkopts_default, which turns off link-time
        optimization); most microbenchmarks produce relatively accurate results that way.
      **kwargs: Passed to cc_test.
    """
    cc_test(
        name = name,
        defines = ["WD_IS_BENCHMARK"],
        linkstatic = 1 if tool else CC_TEST_LINKSTATIC,
        visibility = visibility,
        deps = deps + [
            # Runs each benchmark once under `bazel test`; see bench-main.c++.
            "//src/workerd/tests:bench-main",
            "//src/workerd/tests:bench-tools",
            "//build/deps:linkopts_tool" if tool else "//build/deps:linkopts_default",
            # The kj::setupAsyncIo() seam, linked per binary (see kj_test.bzl).
            "//src/workerd/util:setup-async-io",
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
