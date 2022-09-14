load("//:build/wd_cc_library.bzl", "wd_cc_library")

cc_proto_library(
    name = "jaeger-model",
    deps = [":jaeger-model_proto"],
    visibility = ["//visibility:public"],
)

proto_library(
    name = "jaeger-model_proto",
    srcs = ["jaeger-model.proto"],
    deps = [
        "@com_google_protobuf//:timestamp_proto",
        "@com_google_protobuf//:duration_proto"
    ],
    strip_import_prefix = "/src",
)
