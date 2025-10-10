load("@rules_rust//crate_universe:repositories.bzl", "crate_universe_dependencies")
load("@rules_rust//rust:repositories.bzl", "rules_rust_dependencies", "rust_register_toolchains", "rustfmt_toolchain_repository")
load("@rules_rust//tools/rust_analyzer:deps.bzl", "rust_analyzer_dependencies")
load("//deps/rust/crates:crates.bzl", "crate_repositories")

# We use nightly because of https://github.com/bazelbuild/rules_rust/issues/3459
RUST_VERSION = "nightly/2025-09-18"

# List of allowed unstable features
ALLOWED_FEATURES = []

# rustc flags applied to all workerd source code
RUSTC_FLAGS = [
    # We can't enable this in .bazelrc because 3rd-party crates don't like this.
    # used manually everywhere instead.
    "-Zallow-features=" + ",".join(ALLOWED_FEATURES),
]

# List of additional triples to be configured on top of the local platform triple
RUST_TARGET_TRIPLES = [
    # Add support for macOS cross-compilation
    "x86_64-apple-darwin",
    # Add support for macOS rosetta
    "aarch64-unknown-linux-gnu",
]

def rust_toolchains():
    rules_rust_dependencies()
    rust_register_toolchains(
        edition = "2024",
        extra_target_triples = RUST_TARGET_TRIPLES,
        extra_rustc_flags = {
            # Enable ISA extensions matching the ones used for C++. The clmul feature is included as
            # it is still "unstable" as of 1.86.0.
            "x86_64-unknown-linux-gnu": ["-Ctarget-feature=+sse4.2"],
            "x86_64-apple-darwin": ["-Ctarget-feature=+sse4.2"],
            "x86_64-pc-windows-msvc": ["-Ctarget-feature=+sse4.2"],
            "aarch64-unknown-linux-gnu": ["-Ctarget-feature=+crc"],
            # No options needed for aarch64-apple-darwin: CRC feature is enabled by default.
        },
        versions = [RUST_VERSION],
    )

    for t in RUST_TARGET_TRIPLES:
        rustfmt_toolchain_repository(
            name = "rustfmt_toolchain_" + t,
            exec_triple = t,
            version = RUST_VERSION,
        )

    crate_universe_dependencies()

    # Load rust crate dependencies.
    # These could be regenerated from cargo.bzl by using
    # `just update-rust` (consult `just --list` or justfile for more details)
    crate_repositories()
    rust_analyzer_dependencies()
