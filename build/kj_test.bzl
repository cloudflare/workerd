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
        linkopts = select({
            "@//:use_dead_strip": ["-Wl,-dead_strip", "-Wl,-no_exported_symbols"],
            "//conditions:default": [""],
        }),
        data = data,
        tags = tags,
    )
