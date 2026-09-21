# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

"""Per-host sanitizer-instrumented Rust toolchains (see //build/rust:BUILD.bazel)."""

load("@rules_rust//rust:toolchain.bzl", "rust_toolchain")
load("//build/deps:gen/build_deps.bzl", "RUST_NIGHTLY_DATE", "RUST_NIGHTLY_VERSION")
load(":build_std.bzl", "compiler_runtime_files", "instrumented_rust_std")

# rustc -Zsanitizer value => (runtime library stem, platform constraint).
_SANITIZERS = {
    "address": ("asan", "//build/platforms:sanitizer_address"),
    "thread": ("tsan", "//build/platforms:sanitizer_thread"),
}

# stdlib_linkflags mirror rules_rust's own generated toolchain for each triple, and
# extra_rustc_flags mirror rust.toolchain's extra_rustc_flags_triples (//build/deps:rust.MODULE.bazel).
_HOSTS = {
    "linux_aarch64": struct(
        cpu = "@platforms//cpu:aarch64",
        dylib_ext = ".so",
        extra_rustc_flags = ["-Ctarget-feature=+crc"],
        os = "@platforms//os:linux",
        rust_tools = "@rust_nightly_linux_aarch64",
        sanitizer_rustc_flags = {},
        stdlib_linkflags = [
            "-ldl",
            "-lpthread",
        ],
        target_triple = "aarch64-unknown-linux-gnu",
    ),
    "linux_x86_64": struct(
        cpu = "@platforms//cpu:x86_64",
        dylib_ext = ".so",
        extra_rustc_flags = ["-Ctarget-feature=+sse4.2,+pclmulqdq"],
        os = "@platforms//os:linux",
        rust_tools = "@rust_nightly_linux_x86_64",
        sanitizer_rustc_flags = {},
        stdlib_linkflags = [
            "-ldl",
            "-lpthread",
        ],
        target_triple = "x86_64-unknown-linux-gnu",
    ),
    "macos_aarch64": struct(
        cpu = "@platforms//cpu:aarch64",
        dylib_ext = ".dylib",
        # rustc passes -nodefaultlibs to the linker driver, and unlike Linux, Darwin's clang then
        # also omits the sanitizer runtime that -Zexternal-clangrt (.bazelrc) relies on it to
        # provide. Letting clang add its default libraries restores
        # libclang_rt.<sanitizer>_osx_dynamic.dylib (and its rpath) at every Rust-driven link, so
        # rust_binary/rust_test targets share Clang's runtime with the C++ they link, exactly as on
        # Linux.
        extra_rustc_flags = ["-Cdefault-linker-libraries=yes"],
        os = "@platforms//os:macos",
        rust_tools = "@rust_nightly_macos_aarch64",
        sanitizer_rustc_flags = {
            # The C++ toolchain is Apple Clang, whose ASan runtime exports
            # __asan_version_mismatch_check_apple_clang_<version> in place of upstream LLVM's
            # __asan_version_mismatch_check_v8 that rustc's module constructors reference.
            "address": ["-Cllvm-args=-asan-guard-against-version-mismatch=false"],
        },
        stdlib_linkflags = [
            "-lSystem",
            "-lresolv",
        ],
        target_triple = "aarch64-apple-darwin",
    ),
}

def sanitizer_rust_toolchain(name, sanitizer, host):
    """One host platform's sanitizer-instrumented Rust toolchain.

    Defines `<name>_rustc_lib`, `<name>_std` (the instrumented standard library),
    `<name>_impl` (the rust_toolchain) and `<name>` (the registrable toolchain),
    all constrained to the host's cpu/os plus the sanitizer's platform constraint.

    Args:
        name: toolchain name.
        sanitizer: rustc -Zsanitizer value, "address" or "thread".
        host: key of _HOSTS.
    """
    runtime, constraint = _SANITIZERS[sanitizer]
    h = _HOSTS[host]
    host_constraints = [h.cpu, h.os]
    sanitizer_rustc_flags = h.sanitizer_rustc_flags.get(sanitizer, [])

    compiler_runtime_files(
        name = name + "_rustc_lib",
        src = h.rust_tools + "//:rustc_lib",
        sanitizer = runtime,
        tags = ["manual"],
        target_compatible_with = host_constraints,
    )

    instrumented_rust_std(
        name = name + "_std",
        rust_src = "@rust_nightly_src",
        rust_tools = h.rust_tools,
        rustflags = sanitizer_rustc_flags,
        sanitizer = sanitizer,
        tags = ["manual"],
        target_compatible_with = host_constraints + [constraint],
        target_triple = h.target_triple,
    )

    rust_toolchain(
        name = name + "_impl",
        allocator_library = "@rules_rust//ffi/rs:empty",
        binary_ext = "",
        cargo = h.rust_tools + "//:cargo",
        cargo_clippy = h.rust_tools + "//:cargo_clippy_bin",
        channel = "nightly",
        clippy_driver = h.rust_tools + "//:clippy_driver_bin",
        default_edition = "2024",
        dylib_ext = h.dylib_ext,
        exec_triple = h.target_triple,
        extra_exec_rustc_flags = [],
        extra_rustc_flags = h.extra_rustc_flags + sanitizer_rustc_flags,
        iso_date = RUST_NIGHTLY_DATE,
        linker = h.rust_tools + "//:rust-lld",
        linker_type = "direct",
        llvm_cov = h.rust_tools + "//:llvm_cov_bin",
        llvm_lib = h.rust_tools + "//:llvm_lib",
        llvm_profdata = h.rust_tools + "//:llvm_profdata_bin",
        rust_doc = h.rust_tools + "//:rustdoc",
        rust_objcopy = h.rust_tools + "//:rust-objcopy",
        rust_std = ":" + name + "_std",
        rustc = h.rust_tools + "//:rustc",
        rustc_lib = ":" + name + "_rustc_lib",
        rustfmt = h.rust_tools + "//:rustfmt_bin",
        staticlib_ext = ".a",
        stdlib_linkflags = h.stdlib_linkflags,
        tags = ["manual"],
        target_compatible_with = host_constraints + [constraint],
        target_triple = h.target_triple,
        version = RUST_NIGHTLY_VERSION,
    )

    native.toolchain(
        name = name,
        exec_compatible_with = host_constraints,
        tags = ["manual"],
        target_compatible_with = host_constraints + [constraint],
        target_settings = ["@rules_rust//rust/toolchain/channel:nightly"],
        toolchain = ":" + name + "_impl",
        toolchain_type = "@rules_rust//rust:toolchain",
    )
