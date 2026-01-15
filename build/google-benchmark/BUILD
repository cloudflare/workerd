load("@rules_cc//cc:cc_library.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "benchmark",
    deps = ["@codspeed_google_benchmark_compat//:benchmark"],
)

cc_library(
    name = "benchmark_main",
    deps = ["@codspeed_google_benchmark_compat//:benchmark_main"],
)

alias(
    name = "codspeed_mode",
    actual = "@codspeed_google_benchmark_compat//:codspeed_mode",
)
