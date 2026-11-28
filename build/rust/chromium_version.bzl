"""The Rust release of Chromium's Rust toolchain (build/deps/rust.MODULE.bazel).

It is a constant here, rather than read from the toolchain's repository, so that //build/rust loads
without that repository, as in a module that depends on workerd without its toolchains. The
repository rule fails if the package is of another release.
"""

CHROMIUM_RUST_VERSION = "1.101.0"
