"""Chromium's prebuilt Rust toolchain, for the Rust toolchains of Linux x86_64 hosts (//build/rust).

Chromium publishes the Rust toolchain it builds with, for Linux x86_64 hosts, built against the
LLVM revision of its clang. Rust and C++ then come from one LLVM: lld reads rustc's bitcode under
cross-language LTO, and clang's profile runtime reads the counters rustc writes in coverage and
PGO-instrumented builds. The package has rustc, Cargo, Clippy, rustdoc and rustfmt, the standard
library for x86_64 Linux, and the standard library's sources, from which //build/rust builds the
sanitizer-instrumented ones.

The package's Cargo loads the system's libcurl, which a remote executor may not have, so this rule
replaces it with rust-lang.org's nightly Cargo of the day after the package's rustc, which links
only the C library.
"""

load("//build/rust:chromium_version.bzl", "CHROMIUM_RUST_VERSION")

# Components that no build uses, which make up most of the package's unpacked size: rustc-dev
# holds the compiler's own crates, for tools built against rustc's internals, and
# rust-analyzer-preview an editor's language server.
_UNUSED_COMPONENTS = [
    "rust-analyzer-preview",
    "rustc-dev",
]

# Chromium's own binding generators and their support files, which no component lists. The
# libclang that bindgen loads is deleted by name, which has its version in it.
_UNUSED_FILES = [
    "bin/bindgen",
    "bin/cc_bindings_from_rs",
    "bin/rs_bindings_from_cc",
    "lib/third_party",
]

def _delete_component(rctx, prefix, component):
    """Deletes the files a component's rust-installer manifest lists, and the manifest."""
    manifest = "lib/rustlib/manifest-" + component
    for line in rctx.read(manifest).splitlines():
        # Each line is "file:" followed by the file's path where the package was built.
        path = line.removeprefix("file:")
        if not path.startswith(prefix):
            fail("unexpected entry in %s: %s" % (manifest, line))
        rctx.delete(path.removeprefix(prefix))
    rctx.delete(manifest)

def _chromium_rust_impl(rctx):
    rctx.download_and_extract(url = rctx.attr.url, sha256 = rctx.attr.sha256)

    # The manifests name files by where the package was built; rustc's own entry gives that prefix.
    rustc = [
        line.removeprefix("file:")
        for line in rctx.read("lib/rustlib/manifest-rustc").splitlines()
        if line.endswith("/bin/rustc")
    ]
    if len(rustc) != 1:
        fail("expected one bin/rustc in manifest-rustc, found %s" % rustc)
    prefix = rustc[0].removesuffix("bin/rustc")
    for component in _UNUSED_COMPONENTS:
        _delete_component(rctx, prefix, component)
    for path in _UNUSED_FILES:
        rctx.delete(path)
    for library in rctx.path("lib").readdir():
        if library.basename.startswith("libclang."):
            rctx.delete(library)

    rctx.download_and_extract(
        url = rctx.attr.cargo["url"],
        sha256 = rctx.attr.cargo["sha256"],
        stripPrefix = rctx.attr.cargo["strip_prefix"],
        output = "nightly-cargo",
    )
    rctx.delete("bin/cargo")
    rctx.rename("nightly-cargo/bin/cargo", "bin/cargo")
    rctx.delete("nightly-cargo")

    # VERSION starts with `rustc 1.101.0 <commit> (...)`. The toolchains declare the release.
    version = rctx.read("VERSION").split(" ")[1]
    if version != CHROMIUM_RUST_VERSION:
        fail("the package is of Rust %s; set CHROMIUM_RUST_VERSION in %s to it" % (
            version,
            "build/rust/chromium_version.bzl",
        ))
    rctx.template("BUILD.bazel", rctx.attr._build_file)

    # The package is pinned by hash.
    return rctx.repo_metadata(reproducible = True)

chromium_rust = repository_rule(
    implementation = _chromium_rust_impl,
    attrs = {
        "cargo": attr.string_dict(
            mandatory = True,
            doc = "The url, sha256 and strip_prefix of the Cargo package that replaces the package's.",
        ),
        "sha256": attr.string(mandatory = True),
        "url": attr.string(mandatory = True),
        "_build_file": attr.label(default = "//build/toolchain:BUILD.chromium_rust"),
    },
)
