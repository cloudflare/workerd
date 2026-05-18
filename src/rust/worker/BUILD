load("//:build/kj_test.bzl", "kj_test")
load("//:build/wd_cc_library.bzl", "wd_cc_library")
load("//:build/wd_rust_crate.bzl", "wd_rust_crate")

wd_rust_crate(
    name = "worker",
    cxx_bridge_deps = {
        "ffi.rs": [
            "@workerd//src/workerd/io:worker-interface",
            "@workerd//src/rust/kj",
        ],
        "error.rs": [
            ":ffi.rs@cxx",
        ],
        "kill_switch.rs": [
            ":ffi.rs@cxx",
        ],
        "ok.rs": [
            ":ffi.rs@cxx",
        ],
    },
    cxx_bridge_srcs = [
        "error.rs",
        "ffi.rs",
        "kill_switch.rs",
        "ok.rs",
    ],
    proc_macro_deps = [
        "@crates_vendor//:async-trait",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":bridge",
        "@workerd//src/rust/kj",
        "@workerd//src/workerd/io:outcome_capnp_rust",
    ],
)

wd_cc_library(
    name = "bridge",
    hdrs = ["bridge.h"],
    deps = [
        ":ffi.rs@cxx",
        "@capnp-cpp//src/kj",
        "@capnp-cpp//src/kj/compat:kj-http",
        "@workerd//src/workerd/io:worker-interface",
        "@workerd//src/workerd/util",
    ],
)

kj_test(
    src = "test.c++",
    deps = [
        ":worker",
        "@capnp-cpp//src/kj:kj-async",
        "@capnp-cpp//src/kj/compat:kj-http",
        "@workerd//src/workerd/io",
        "@workerd//src/workerd/io:frankenvalue",
    ],
)
