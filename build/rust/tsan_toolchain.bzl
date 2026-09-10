# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

"""Per-host ThreadSanitizer-instrumented Rust toolchains (see //build/rust:BUILD.bazel)."""

load("@rules_rust//rust:toolchain.bzl", "rust_toolchain")
load("//build/deps:gen/build_deps.bzl", "RUST_NIGHTLY_DATE", "RUST_NIGHTLY_VERSION")
load(":build_std.bzl", "compiler_runtime_files", "instrumented_rust_std")

def tsan_rust_toolchain(
        name,
        rust_tools,
        target_triple,
        cpu,
        os,
        dylib_ext,
        stdlib_linkflags,
        extra_rustc_flags = []):
    """One host platform's ThreadSanitizer-instrumented Rust toolchain.

    Defines `<name>_rustc_lib`, `<name>_std` (the TSan-built standard library),
    `<name>_impl` (the rust_toolchain) and `<name>` (the registrable toolchain),
    all constrained to `cpu`/`os` plus the sanitizer_thread platform constraint.
    """
    host = [cpu, os]

    compiler_runtime_files(
        name = name + "_rustc_lib",
        src = rust_tools + "//:rustc_lib",
        sanitizer = "tsan",
        tags = ["manual"],
        target_compatible_with = host,
    )

    instrumented_rust_std(
        name = name + "_std",
        rust_src = "@rust_nightly_src",
        rust_tools = rust_tools,
        sanitizer = "thread",
        tags = ["manual"],
        target_compatible_with = host + ["//build/platforms:sanitizer_thread"],
        target_triple = target_triple,
    )

    rust_toolchain(
        name = name + "_impl",
        allocator_library = "@rules_rust//ffi/rs:empty",
        binary_ext = "",
        cargo = rust_tools + "//:cargo",
        cargo_clippy = rust_tools + "//:cargo_clippy_bin",
        channel = "nightly",
        clippy_driver = rust_tools + "//:clippy_driver_bin",
        default_edition = "2024",
        dylib_ext = dylib_ext,
        exec_triple = target_triple,
        extra_exec_rustc_flags = [],
        extra_rustc_flags = extra_rustc_flags,
        iso_date = RUST_NIGHTLY_DATE,
        linker = rust_tools + "//:rust-lld",
        linker_type = "direct",
        llvm_cov = rust_tools + "//:llvm_cov_bin",
        llvm_lib = rust_tools + "//:llvm_lib",
        llvm_profdata = rust_tools + "//:llvm_profdata_bin",
        rust_doc = rust_tools + "//:rustdoc",
        rust_objcopy = rust_tools + "//:rust-objcopy",
        rust_std = ":" + name + "_std",
        rustc = rust_tools + "//:rustc",
        rustc_lib = ":" + name + "_rustc_lib",
        rustfmt = rust_tools + "//:rustfmt_bin",
        staticlib_ext = ".a",
        stdlib_linkflags = stdlib_linkflags,
        tags = ["manual"],
        target_compatible_with = host + ["//build/platforms:sanitizer_thread"],
        target_triple = target_triple,
        version = RUST_NIGHTLY_VERSION,
    )

    native.toolchain(
        name = name,
        exec_compatible_with = host,
        tags = ["manual"],
        target_compatible_with = host + ["//build/platforms:sanitizer_thread"],
        target_settings = ["@rules_rust//rust/toolchain/channel:nightly"],
        toolchain = ":" + name + "_impl",
        toolchain_type = "@rules_rust//rust:toolchain",
    )
