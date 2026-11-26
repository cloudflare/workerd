"""Shared C++ linking configuration."""

# C++ libraries linked into Rust use dynamic linkage where supported, but Windows requires static
# linkage.
CC_LIBRARY_LINKSTATIC = select({
    "@platforms//os:windows": True,
    "//conditions:default": False,
})

# C++ toolchain features for statically linked binaries that aren't testonly but don't need full
# ThinLTO, such as development tools. Downstream toolchains then use the shared non-LTO backends of
# their dependencies, as they do for test binaries; toolchains that don't define the feature ignore
# it.
# Ref: https://github.com/bazelbuild/rules_cc/blob/0.2.27/docs/features.md#lto-features
SHARED_NONLTO_BACKENDS_FEATURES = ["thin_lto_all_linkstatic_use_shared_nonlto_backends"]

# C++ toolchain features for a test that must run with the control-flow integrity checks of a
# downstream release build. Its toolchain links test binaries through shared non-LTO backends,
# which replace the CFI type tests with true; this links the test with full ThinLTO, as a release
# binary is, so that an indirect call that cfi-icall rejects fails the test there. Toolchains that
# don't define the feature ignore it.
CFI_CHECKED_TEST_FEATURES = ["-thin_lto_linkstatic_tests_use_shared_nonlto_backends"]

# Tests use dynamic linkage on Linux, where Bazel supports it, to reduce link time and binary size.
# TSan instead links statically so every object uses the instrumented static libc++ and libc++abi.
CC_TEST_LINKSTATIC = select({
    "@workerd//build/platforms:sanitizer_thread_linux": 1,
    "@platforms//os:linux": 0,
    "//conditions:default": 1,
})
