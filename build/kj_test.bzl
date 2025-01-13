def kj_test(
        src,
        data = [],
        deps = [],
        tags = [],
        size = "medium"):
    test_name = src.removesuffix(".c++")
    native.cc_test(
        name = test_name,
        srcs = [src],
        deps = [
            "@capnp-cpp//src/kj:kj-test",
        ] + deps,
        dynamic_deps = [
            "@workerd-v8//:v8-shared",
        ],
        linkopts = select({
            "@//:use_dead_strip": ["-Wl,-dead_strip", "-Wl,-no_exported_symbols"],
            "//conditions:default": [""],
        }),
        data = data,
        tags = tags,
        size = size,
    )
