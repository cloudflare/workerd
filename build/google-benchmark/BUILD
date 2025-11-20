load("@rules_cc//cc:cc_library.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "benchmark",
    deps = ["@codspeed//:benchmark"],
)

cc_library(
    name = "benchmark_main",
    deps = ["@codspeed//:benchmark_main"],
)

alias(
    name = "codspeed_mode",
    actual = "@codspeed//:codspeed_mode",
)
