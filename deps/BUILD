alias(
    name = "rust_runtime",
    actual = "//deps/rust:runtime",
    tags = ["off-by-default"],
    target_compatible_with = select({
        "//build/config:no_build": ["@platforms//:incompatible"],
        "//conditions:default": [],
    }),
    visibility = ["//visibility:public"],
)
