# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

"""Per-host sanitizer-instrumented Rust toolchains (see //build/rust:BUILD.bazel)."""

load("@rules_rust//cargo:cargo_toolchain.bzl", "cargo_toolchain")
load("@rules_rust//rust:toolchain.bzl", "rust_toolchain")
load("//build/deps:gen/build_deps.bzl", "RUST_NIGHTLY_DATE", "RUST_NIGHTLY_VERSION")
load(":build_std.bzl", "compiler_runtime_files", "rust_std_from_source")
load(":chromium_version.bzl", "CHROMIUM_RUST_VERSION")

# rustc -Zsanitizer value => (runtime library stem, platform constraint).
_SANITIZERS = {
    "address": ("asan", "//build/platforms:sanitizer_address"),
    "thread": ("tsan", "//build/platforms:sanitizer_thread"),
}

# The toolchain a host builds with, and the standard library sources it instruments: rules_rust's
# nightly toolchain for the host.
def _nightly(rust_tools, target_triple):
    return {
        "env": {},
        "iso_date": RUST_NIGHTLY_DATE,
        "linker": rust_tools + "//:rust-lld",
        "llvm_cov": rust_tools + "//:llvm_cov_bin",
        "llvm_lib": rust_tools + "//:llvm_lib",
        "llvm_profdata": rust_tools + "//:llvm_profdata_bin",
        "rust_src": "@rust_nightly_src//:rust_src",
        "rust_src_manifest": "@rust_nightly_src//:library/Cargo.toml",
        # rustc's libraries, and the prebuilt standard library and self-contained linker that the
        # build scripts link with.
        "rust_toolchain_files": [
            rust_tools + "//:rustc_lib",
            rust_tools + "//:rust-lld",
            rust_tools + "//:rust_std-" + target_triple,
        ],
        "rust_tools": rust_tools,
        "version": RUST_NIGHTLY_VERSION,
    }

# Chromium's toolchain, which Linux x86_64 hosts build all Rust with (//build/rust:BUILD.bazel). It
# is marked stable, so the sanitizers' unstable options need RUSTC_BOOTSTRAP. Its sources need no
# patch: the TSan fcntl patch works around a TSan that its LLVM revision has fixed.
_CHROMIUM = {
    "env": {"RUSTC_BOOTSTRAP": "1"},
    "iso_date": None,
    "linker": None,
    "llvm_cov": "@chromium_clang//:bin/llvm-cov",
    "llvm_lib": None,
    "llvm_profdata": "@chromium_clang//:bin/llvm-profdata",
    "rust_src": "@chromium_rust//:rust_src",
    "rust_src_manifest": "@chromium_rust//:lib/rustlib/src/rust/library/Cargo.toml",
    "rust_toolchain_files": [
        "@chromium_rust//:rustc_lib",
        "@chromium_rust//:rust_std-x86_64-unknown-linux-gnu",
    ],
    "rust_tools": "@chromium_rust",
    "version": CHROMIUM_RUST_VERSION,
}

# stdlib_linkflags mirror rules_rust's own generated toolchain for each triple, and
# extra_rustc_flags mirror rust.toolchain's extra_rustc_flags_triples (//build/deps:rust.MODULE.bazel).
_HOSTS = {
    "linux_aarch64": struct(
        cpu = "@platforms//cpu:aarch64",
        dylib_ext = ".so",
        extra_rustc_flags = ["-Ctarget-feature=+crc"],
        os = "@platforms//os:linux",
        sanitizer_rustc_flags = {},
        stdlib_linkflags = [
            "-ldl",
            "-lpthread",
        ],
        target_triple = "aarch64-unknown-linux-gnu",
        **_nightly("@rust_nightly_linux_aarch64", "aarch64-unknown-linux-gnu")
    ),
    "linux_x86_64": struct(
        cpu = "@platforms//cpu:x86_64",
        dylib_ext = ".so",
        extra_rustc_flags = ["-Ctarget-feature=+sse4.2,+pclmulqdq"],
        os = "@platforms//os:linux",
        sanitizer_rustc_flags = {},
        stdlib_linkflags = [
            "-ldl",
            "-lpthread",
        ],
        target_triple = "x86_64-unknown-linux-gnu",
        **_CHROMIUM
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
        **_nightly("@rust_nightly_macos_aarch64", "aarch64-apple-darwin")
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

    rust_std_from_source(
        name = name + "_std",
        rust_src = h.rust_src,
        rust_src_manifest = h.rust_src_manifest,
        rust_toolchain_files = h.rust_toolchain_files,
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
        cargo_clippy = h.rust_tools + "//:cargo_clippy_bin",
        channel = "nightly",
        clippy_driver = h.rust_tools + "//:clippy_driver_bin",
        default_edition = "2024",
        dylib_ext = h.dylib_ext,
        env = h.env,
        exec_triple = h.target_triple,
        extra_exec_rustc_flags = [],
        extra_rustc_flags = h.extra_rustc_flags + sanitizer_rustc_flags,
        iso_date = h.iso_date,
        linker = h.linker,
        linker_type = "direct" if h.linker else None,
        llvm_cov = h.llvm_cov,
        llvm_lib = h.llvm_lib,
        llvm_profdata = h.llvm_profdata,
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
        version = h.version,
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

    cargo_toolchain(
        name = name + "_cargo_impl",
        cargo = h.rust_tools + "//:cargo",
        channel = "nightly",
        iso_date = h.iso_date,
        tags = ["manual"],
        version = h.version,
    )

    native.toolchain(
        name = name + "_cargo",
        exec_compatible_with = host_constraints,
        tags = ["manual"],
        target_compatible_with = host_constraints + [constraint],
        target_settings = ["@rules_rust//rust/toolchain/channel:nightly"],
        toolchain = ":" + name + "_cargo_impl",
        toolchain_type = "@rules_rust//cargo:toolchain_type",
    )
