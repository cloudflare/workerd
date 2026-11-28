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

# Tests use dynamic linkage on Linux and macOS, where Bazel supports it, to reduce link time and
# binary size: under ThinLTO, each library is then optimized once, in its own shared library, rather
# than again in every test binary. macOS needs a patch to apple_support
# (patches/apple_support/0001-Support-the-dynamic-linker-on-macOS.patch). On Linux, TSan instead
# links statically so every object uses the instrumented static libc++ and libc++abi.
CC_TEST_LINKSTATIC = select({
    "@workerd//build/platforms:sanitizer_thread_linux": 1,
    "@platforms//os:linux": 0,
    "@platforms//os:macos": 0,
    "//conditions:default": 1,
})

# The link options of dynamically linked test binaries. On macOS, each library's dylib resolves the
# symbols it uses from other libraries when the test loads it, some of them from the test binary,
# which links the static libraries, such as the Rust code's. So the test binary exports its symbols
# (linkopts_default leaves out -no_exported_symbols) and keeps them all, through -dead_strip and
# ThinLTO (-export_dynamic). The macOS linker, unlike ELF linkers, doesn't take an object from an
# archive when only a dylib needs it; MACOS_ALWAYSLINK makes the test binary take all of those.
CC_TEST_LINKOPTS = select({
    "@platforms//os:macos": ["-Wl,-export_dynamic"],
    "//conditions:default": [],
})

# alwayslink for the static libraries whose symbols the dylibs of macOS tests use (see
# CC_TEST_LINKOPTS): workerd's Rust crates, their cxx bridges and cxx's C++ runtime, as well as
# perfetto's static libraries (build/perfetto/perfetto_cfg.bzl), zstd (build/deps:zstd), the
# temporal_capi crate (deps/rust:temporal_capi_crate) and, for lol_html's Rust code, an object that
# refers to its C API (deps/rust/BUILD.lolhtml). Binaries then load all of each library's objects
# (-force_load), which -all_load would do for every archive, including the Rust standard library's,
# which deps/rust:runtime's static library holds a second copy of.
MACOS_ALWAYSLINK = select({
    "@platforms//os:macos": True,
    "//conditions:default": False,
})

# CC_TEST_LINKSTATIC for tests that can't link dynamically on macOS: those whose libraries read a
# thread_local that another library defines, such as protobuf's ThreadSafeArena::thread_cache_. A
# dylib that resolves its symbols at load time can't do that, as the macOS linker must know that a
# symbol is thread-local to link the code that reads it.
CC_TEST_LINKSTATIC_WITHOUT_MACOS_DYLIBS = select({
    "@workerd//build/platforms:sanitizer_thread_linux": 1,
    "@platforms//os:linux": 0,
    "//conditions:default": 1,
})
