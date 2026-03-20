"""
Repository rule that downloads a standalone rustfmt binary along with the rustc
shared libraries it depends on.

This avoids needing `bazel run @rules_rust//:rustfmt`, which triggers C++ toolchain
resolution and (in edgeworker) the V8 build cache download — a multi-GB fetch that
is unnecessary for code formatting.
"""

load("@@//:build/http_proxy_config.bzl", "PROXY_URL")

_URL_TEMPLATE = "https://static.rust-lang.org/dist/{iso_date}/{component}-nightly-{triple}.tar.xz"

def _rustfmt_standalone_repository_impl(repository_ctx):
    triple = repository_ctx.attr.triple
    iso_date = repository_ctx.attr.iso_date
    proxy = PROXY_URL

    # Download the rustfmt component (contains bin/rustfmt and bin/cargo-fmt).
    rustfmt_url = proxy + _URL_TEMPLATE.format(
        iso_date = iso_date,
        component = "rustfmt",
        triple = triple,
    )
    repository_ctx.download_and_extract(
        url = rustfmt_url,
        sha256 = repository_ctx.attr.rustfmt_sha256,
        stripPrefix = "rustfmt-nightly-{}/rustfmt-preview".format(triple),
    )

    # Download the rustc component (contains shared libraries that rustfmt
    # dynamically links against, e.g. librustc_driver-*.so / .dylib).
    rustc_url = proxy + _URL_TEMPLATE.format(
        iso_date = iso_date,
        component = "rustc",
        triple = triple,
    )
    repository_ctx.download_and_extract(
        url = rustc_url,
        sha256 = repository_ctx.attr.rustc_sha256,
        stripPrefix = "rustc-nightly-{}/rustc".format(triple),
    )

    # Create a wrapper script that sets the library path so the dynamically-linked
    # rustfmt binary can find its libraries.  We embed absolute paths because the
    # wrapper may be copied to bazel-bin by native_binary.
    repo_dir = str(repository_ctx.path(""))
    is_macos = "apple-darwin" in triple
    if is_macos:
        wrapper = """\
#!/bin/bash
DYLD_LIBRARY_PATH="{lib_dir}:${{DYLD_LIBRARY_PATH:-}}" exec "{rustfmt_bin}" "$@"
"""
    else:
        wrapper = """\
#!/bin/bash
LD_LIBRARY_PATH="{lib_dir}:${{LD_LIBRARY_PATH:-}}" exec "{rustfmt_bin}" "$@"
"""
    repository_ctx.file("rustfmt", content = wrapper.format(
        lib_dir = repo_dir + "/lib",
        rustfmt_bin = repo_dir + "/bin/rustfmt",
    ), executable = True)

    repository_ctx.file("BUILD.bazel", 'exports_files(["rustfmt"])\n')

rustfmt_standalone_repository = repository_rule(
    doc = "Downloads a standalone rustfmt binary and the rustc shared libraries it needs.",
    attrs = {
        "triple": attr.string(mandatory = True, doc = "Rust target triple, e.g. x86_64-unknown-linux-gnu"),
        "iso_date": attr.string(mandatory = True, doc = "Nightly date, e.g. 2026-02-12"),
        "rustfmt_sha256": attr.string(mandatory = True, doc = "SHA-256 of the rustfmt component tarball"),
        "rustc_sha256": attr.string(mandatory = True, doc = "SHA-256 of the rustc component tarball"),
    },
    implementation = _rustfmt_standalone_repository_impl,
)

# -- Module extension --

def _rustfmt_standalone_ext_impl(module_ctx):
    for mod in module_ctx.modules:
        for repo in mod.tags.repo:
            rustfmt_standalone_repository(
                name = repo.name,
                triple = repo.triple,
                iso_date = repo.iso_date,
                rustfmt_sha256 = repo.rustfmt_sha256,
                rustc_sha256 = repo.rustc_sha256,
            )
    return module_ctx.extension_metadata(
        root_module_direct_deps = "all",
        root_module_direct_dev_deps = [],
    )

_repo_tag = tag_class(attrs = {
    "name": attr.string(mandatory = True),
    "triple": attr.string(mandatory = True),
    "iso_date": attr.string(mandatory = True),
    "rustfmt_sha256": attr.string(mandatory = True),
    "rustc_sha256": attr.string(mandatory = True),
})

rustfmt_standalone = module_extension(
    implementation = _rustfmt_standalone_ext_impl,
    tag_classes = {"repo": _repo_tag},
)
