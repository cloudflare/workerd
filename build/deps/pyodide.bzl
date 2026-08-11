load("@workerd//build/deps:dep_pyodide.bzl", "dep_pyodide")

def _pyodide_impl(module_ctx):
    direct_deps = dep_pyodide()
    return module_ctx.extension_metadata(
        root_module_direct_deps = direct_deps,
        root_module_direct_dev_deps = [],
    )

pyodide = module_extension(
    implementation = _pyodide_impl,
)
