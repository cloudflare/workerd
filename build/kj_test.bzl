load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_shell//shell:sh_test.bzl", "sh_test")
load("//:build/linking.bzl", "CC_TEST_LINKSTATIC")

def kj_test(
        src,
        data = [],
        deps = [],
        tags = [],
        size = "medium",
        target_compatible_with = None,
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
            # The tokio kj::setupAsyncIo() is linked per BINARY: every test binary links it here, and
            # no library carries it, so a binary can never end up with two event-loop
            # implementations in its link.
            "//src/workerd/util:setup-async-io",
        ] + deps,
        linkstatic = CC_TEST_LINKSTATIC,
        data = data,
        # Tag with cpu:2 since this target depends on linkopts_default.
        tags = tags + ["cpu:2"],
        target_compatible_with = target_compatible_with,
        **kwargs
    )

    sh_test(
        name = test_name + "@",
        srcs = ["//build/fixtures:kj_test.sh"],
        args = ["$(location {})".format(binary_name)],
        data = data + [binary_name],
        tags = tags,
        size = size,
        target_compatible_with = target_compatible_with,
    )

    sh_test(
        name = test_name + "@all-autogates",
        srcs = ["//build/fixtures:kj_test.sh"],
        args = ["$(location {})".format(binary_name)],
        data = data + [binary_name],
        env = {"WORKERD_ALL_AUTOGATES": "1"},
        # Tag with no-coverage to reduce coverage CI time
        tags = tags + ["no-coverage"],
        size = size,
        target_compatible_with = target_compatible_with,
    )
