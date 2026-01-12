"""Module extensions for using vendored crates with bzlmod. Derived from https://github.com/bazelbuild/rules_rust/blob/0.68.1/examples/hello_world/third-party-in-workspace/extension.bzl. """

load("//deps/rust/crates:crates.bzl", _crate_repositories = "crate_repositories")

def _crate_repositories_impl(module_ctx):
    _crate_repositories()
    return module_ctx.extension_metadata(
        root_module_direct_deps = ["crates_vendor", "crates_vendor__lol_html_c_api-1.3.0"],
        root_module_direct_dev_deps = [],
    )

crate_repositories = module_extension(
    implementation = _crate_repositories_impl,
)
