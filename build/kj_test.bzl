load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_shell//shell:sh_test.bzl", "sh_test")
load("//:build/rust_io_backend.bzl", "rust_io_backend_local_defines")

def kj_test(
        src,
        data = [],
        deps = [],
        tags = [],
        size = "medium",
        local_defines = [],
        **kwargs):
    test_name = src.removesuffix(".c++")
    binary_name = test_name + "_binary"
    cc_binary(
        name = binary_name,
        testonly = True,
        srcs = [src],
        deps = [
            "@capnp-cpp//src/kj:kj-test",
            "//build/deps:linkopts_default",
        ] + deps,
        # Under --//:io_backend=rust the native kj::setupAsyncIo()/UnixEventPort aren't linked, so
        # tests that need an event loop swap to the tokio backend behind
        # `#if WORKERD_RUST_IO_BACKEND_RUST`. Supplied to every kj_test TU here (cheap, harmless
        # where unused) instead of repeating it per target.
        local_defines = local_defines + rust_io_backend_local_defines(),
        linkstatic = select({
            "@platforms//os:linux": 0,
            "//conditions:default": 1,
        }),
        data = data,
        tags = tags,
        target_compatible_with = select({
            "@//build/config:no_build": ["@platforms//:incompatible"],
            "//conditions:default": [],
        }),
        **kwargs
    )

    pkg = native.package_name()
    cross_alias = binary_name + "_cross"
    native.alias(
        name = cross_alias,
        actual = select({
            "@//build/config:prebuilt_binaries_arm64": "@//:bin.arm64/tmp/workerd/{}/{}.aarch64-linux-gnu".format(pkg, binary_name),
            "//conditions:default": binary_name,
        }),
    )

    sh_test(
        name = test_name + "@",
        srcs = ["//build/fixtures:kj_test.sh"],
        args = ["$(location {})".format(cross_alias)],
        data = data + [cross_alias],
        tags = tags,
        size = size,
    )

    sh_test(
        name = test_name + "@all-autogates",
        srcs = ["//build/fixtures:kj_test.sh"],
        args = ["$(location {})".format(cross_alias)],
        data = data + [cross_alias],
        env = {"WORKERD_ALL_AUTOGATES": "1"},
        # Tag with no-coverage to reduce coverage CI time
        tags = tags + ["no-coverage"],
        size = size,
    )
