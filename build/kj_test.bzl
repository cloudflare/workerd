def kj_test(
        src,
        data = [],
        deps = [],
        tags = []):
    test_name = src.removesuffix(".c++")
    native.cc_test(
        name = test_name,
        srcs = [src],
        deps = ["@capnp-cpp//src/kj:kj-test"] + deps,
    )
