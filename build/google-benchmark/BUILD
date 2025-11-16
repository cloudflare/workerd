load("@rules_cc//cc:cc_library.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "benchmark",
    deps = ["@codspeed//google_benchmark:benchmark"],
)

cc_library(
    name = "benchmark_main",
    deps = ["@codspeed//google_benchmark:benchmark_main"],
)

alias(
    name = "codspeed_mode",
    actual = "@codspeed//core:codspeed_mode",
)
