load("//:build/kj_test.bzl", "kj_test")
load("//:build/wd_cc_library.bzl", "wd_cc_library")
load("//:build/wd_rust_crate.bzl", "wd_rust_crate")

wd_rust_crate(
    name = "worker",
    cxx_bridge_deps = {
        "ffi.rs": [
            "@workerd//src/workerd/io:worker-interface-hdrs",
            "@workerd//src/rust/kj",
        ],
        "error.rs": [
            ":ffi.rs@cxx",
        ],
        "failure.rs": [
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
        "failure.rs",
        "ffi.rs",
        "kill_switch.rs",
        "ok.rs",
    ],
    # WorkerInterface::fromException() makes the worker of failure.rs.
    cxx_bridge_visibility = ["//src/workerd/io:__pkg__"],
    link_deps = [
        ":bridge",
    ],
    proc_macro_deps = [
        "@crates_vendor//async-trait",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "@crates_vendor//futures",
        "@workerd//src/rust/kj",
        "@workerd//src/workerd/io:outcome_capnp_rust",
    ],
)

wd_cc_library(
    name = "bridge",
    srcs = ["ffi.c++"],
    hdrs = ["bridge.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":ffi.rs@cxx",
        "@capnp-cpp//src/capnp",
        "@capnp-cpp//src/kj",
        "@capnp-cpp//src/kj/compat:kj-http",
        "@workerd//src/workerd/io:bench_capnp",
        "@workerd//src/workerd/io:worker-interface-hdrs",
        "@workerd//src/workerd/util",
    ],
)

kj_test(
    src = "test.c++",
    deps = [
        ":worker",
        "@capnp-cpp//src/kj:kj-async-core",
        "@capnp-cpp//src/kj:kj-async-io",
        "@capnp-cpp//src/kj/compat:kj-http",
        "@workerd//src/workerd/io",
        "@workerd//src/workerd/io:frankenvalue",
    ],
)
