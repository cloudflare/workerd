load("@rules_cc//cc:cc_library.bzl", "cc_library")

cc_library(
    name = "v8",
    defines = ["WORKERD_ICU_DATA_EMBED"],
    visibility = ["//visibility:public"],
    deps = [
        "@workerd//:icudata-embed",
        "@workerd//:v8_icu",
    ],
)
