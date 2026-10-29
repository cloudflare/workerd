"""Shared C++ linking configuration."""

# C++ libraries linked into Rust use dynamic linkage where supported, but Windows requires static
# linkage.
CC_LIBRARY_LINKSTATIC = select({
    "@platforms//os:windows": True,
    "//conditions:default": False,
})

# C++ toolchain features for binaries that link with //build/deps:linkopts_default. Downstream
# toolchains also use reduced_lto_backend_optimization to tune ThinLTO backend actions; toolchains
# that don't define a feature ignore it.
LINKOPTS_DEFAULT_FEATURES = ["reduced_lto_backend_optimization"]

# C++ toolchain features for statically linked binaries that aren't testonly but don't need full
# ThinLTO, such as development tools. Downstream toolchains then use the shared non-LTO backends of
# their dependencies, as they do for test binaries; toolchains that don't define the feature ignore
# it.
# Ref: https://github.com/bazelbuild/rules_cc/blob/0.2.27/docs/features.md#lto-features
SHARED_NONLTO_BACKENDS_FEATURES = ["thin_lto_all_linkstatic_use_shared_nonlto_backends"]

# Tests use dynamic linkage on Linux, where Bazel supports it, to reduce link time and binary size.
# TSan instead links statically so every object uses the instrumented static libc++ and libc++abi.
CC_TEST_LINKSTATIC = select({
    "@workerd//build/platforms:sanitizer_thread_linux": 1,
    "@platforms//os:linux": 0,
    "//conditions:default": 1,
})
