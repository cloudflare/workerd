"""Shared C++ linking configuration."""

# C++ libraries linked into Rust use dynamic linkage where supported, but Windows requires static
# linkage.
CC_LIBRARY_LINKSTATIC = select({
    "@platforms//os:windows": True,
    "//conditions:default": False,
})

# Tests use dynamic linkage on Linux, where Bazel supports it, to reduce link time and binary size.
# TSan instead links statically so every object uses the instrumented static libc++ and libc++abi.
CC_TEST_LINKSTATIC = select({
    "@workerd//build/platforms:sanitizer_thread_linux": 1,
    "@platforms//os:linux": 0,
    "//conditions:default": 1,
})
