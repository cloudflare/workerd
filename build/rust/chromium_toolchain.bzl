"""The Rust toolchains of Linux x86_64 hosts, from Chromium's toolchain.

Chromium's rustc (//build/toolchain:chromium_rust.bzl) comes from the LLVM revision of the C++
toolchain's clang (build/deps/llvm.MODULE.bazel), so Rust's bitcode and profile counters are those
of the C++ code. Every Rust crate of a build, its proc macros and build scripts included, must come
from one rustc, so these toolchains cover every target that Linux x86_64 hosts build for: x86_64
Linux with the package's standard library, and arm64 Linux with one built from its sources.
build/deps/rust.MODULE.bazel registers them ahead of rules_rust's toolchains, which other hosts use.
"""

load("@rules_rust//cargo:cargo_toolchain.bzl", "cargo_toolchain")
load("@rules_rust//rust:toolchain.bzl", "rust_toolchain")
load(":build_std.bzl", "rust_std_from_source")
load(":chromium_version.bzl", "CHROMIUM_RUST_VERSION")

_HOST_TRIPLE = "x86_64-unknown-linux-gnu"

_HOST_CONSTRAINTS = [
    "@platforms//cpu:x86_64",
    "@platforms//os:linux",
]

def chromium_rust_toolchain(name, cpu, target_triple, extra_rustc_flags):
    """A Linux x86_64 host's Rust and Cargo toolchains for one Linux target.

    Args:
        name: the registrable Rust toolchain's name; `<name>_impl` is the rust_toolchain, and
            `<name>_cargo` the registrable Cargo toolchain, which rules_rust requires from the
            rustc's release.
        cpu: the target's @platforms//cpu constraint name.
        target_triple: the target's triple.
        extra_rustc_flags: rustc flags for the target, those that rust.toolchain() gives it in
            build/deps/rust.MODULE.bazel.
    """
    target_constraints = [
        "@platforms//cpu:" + cpu,
        "@platforms//os:linux",
    ]
    if target_triple == _HOST_TRIPLE:
        rust_std = "@chromium_rust//:rust_std-" + target_triple
    else:
        rust_std = ":" + name + "_std"
        rust_std_from_source(
            name = name + "_std",
            host_triple = _HOST_TRIPLE,
            rust_src = "@chromium_rust//:rust_src",
            rust_src_manifest = "@chromium_rust//:lib/rustlib/src/rust/library/Cargo.toml",
            # The build scripts link with the host's prebuilt standard library.
            rust_toolchain_files = [
                "@chromium_rust//:rustc_lib",
                "@chromium_rust//:rust_std-" + _HOST_TRIPLE,
            ],
            rust_tools = "@chromium_rust",
            tags = ["manual"],
            target_compatible_with = target_constraints,
            target_triple = target_triple,
        )

    rust_toolchain(
        name = name + "_impl",
        allocator_library = "@rules_rust//ffi/rs:empty",
        binary_ext = "",
        cargo = "@chromium_rust//:cargo",
        cargo_clippy = "@chromium_rust//:cargo_clippy_bin",
        channel = "stable",
        clippy_driver = "@chromium_rust//:clippy_driver_bin",
        default_edition = "2024",
        dylib_ext = ".so",
        exec_triple = _HOST_TRIPLE,
        extra_rustc_flags = extra_rustc_flags,
        llvm_cov = "@chromium_clang//:bin/llvm-cov",
        llvm_profdata = "@chromium_clang//:bin/llvm-profdata",
        rust_doc = "@chromium_rust//:rustdoc",
        rust_objcopy = "@chromium_rust//:rust-objcopy",
        rust_std = rust_std,
        rustc = "@chromium_rust//:rustc",
        rustc_lib = "@chromium_rust//:rustc_lib",
        rustfmt = "@chromium_rust//:rustfmt_bin",
        staticlib_ext = ".a",
        stdlib_linkflags = [
            "-ldl",
            "-lpthread",
        ],
        tags = ["manual"],
        target_triple = target_triple,
        version = CHROMIUM_RUST_VERSION,
    )

    native.toolchain(
        name = name,
        exec_compatible_with = _HOST_CONSTRAINTS,
        target_compatible_with = target_constraints,
        toolchain = ":" + name + "_impl",
        toolchain_type = "@rules_rust//rust:toolchain_type",
    )

    cargo_toolchain(
        name = name + "_cargo_impl",
        cargo = "@chromium_rust//:cargo",
        channel = "stable",
        tags = ["manual"],
        version = CHROMIUM_RUST_VERSION,
    )

    native.toolchain(
        name = name + "_cargo",
        exec_compatible_with = _HOST_CONSTRAINTS,
        target_compatible_with = target_constraints,
        toolchain = ":" + name + "_cargo_impl",
        toolchain_type = "@rules_rust//cargo:toolchain_type",
    )
