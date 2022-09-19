load("//:build/kj_test.bzl", "kj_test")
load("//:build/wd_cc_library.bzl", "wd_cc_library")
load("//:build/wd_cc_binary.bzl", "wd_cc_binary")
load("//:build/wd_cc_capnp_library.bzl", "wd_cc_capnp_library")

wd_cc_binary(
    name = "workerd",
    srcs = ["workerd.c++"],
    tags = ["no-arm64"],
    visibility = ["//visibility:public"],
    deps = [
        ":server",
        ":workerd_capnp",
        ":workerd-meta_capnp",
    ],
)

wd_cc_library(
    name = "server",
    srcs = [
        "server.c++",
        "workerd-api.c++",
    ],
    hdrs = [
        "server.h",
        "workerd-api.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":workerd_capnp",
        "@capnp-cpp//src/capnp:capnpc",
        "//src/workerd/io",
        "//src/workerd/jsg",
    ],
)

wd_cc_capnp_library(
    name = "workerd_capnp",
    srcs = ["workerd.capnp"],
    visibility = ["//visibility:public"],
    deps = [
    ],
)

wd_cc_capnp_library(
    name = "workerd-meta_capnp",
    srcs = ["workerd-meta.capnp"],
    visibility = ["//visibility:public"],
    deps = [
        ":workerd_capnp",
    ],
)

[kj_test(
    src = f,
    deps = [
        ":server",
        "//src/workerd/util:test-util",
    ],
) for f in glob(["*-test.c++"])]
