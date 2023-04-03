def kj_test(
        src,
        data = [],
        deps = [],
        tags = []):
    test_name = src.removesuffix(".c++")
    native.cc_test(
        name = test_name,
        srcs = [src],
        deps = [
            "@capnp-cpp//src/kj:kj-test",
        ] + select({
            "@platforms//os:windows": [],
            "//conditions:default": ["@workerd//src/workerd/util:symbolizer"],
        }) + deps,
    )
