load("@rules_rust//crate_universe:repositories.bzl", "crate_universe_dependencies")
load("@rules_rust//rust:repositories.bzl", "rules_rust_dependencies", "rust_register_toolchains", "rustfmt_toolchain_repository")
load("@rules_rust//tools/rust_analyzer:deps.bzl", "rust_analyzer_dependencies")
load("//deps/rust/crates:crates.bzl", "crate_repositories")

RUST_STABLE_VERSION = "1.86.0"  # LLVM 19

RUST_NIGHTLY_VERSION = "nightly/2025-02-20"

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
        versions = [
            RUST_STABLE_VERSION,
            RUST_NIGHTLY_VERSION,
        ],
    )

    for t in RUST_TARGET_TRIPLES:
        rustfmt_toolchain_repository(
            name = "rustfmt_toolchain_" + t,
            exec_triple = t,
            version = RUST_NIGHTLY_VERSION,
        )

    crate_universe_dependencies()

    # Load rust crate dependencies.
    # These could be regenerated from cargo.bzl by using
    # `just update-rust` (consult `just --list` or justfile for more details)
    crate_repositories()
    rust_analyzer_dependencies()
