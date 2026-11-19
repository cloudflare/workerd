"""Macro that builds pyodide_static, the Python Workers JavaScript glue compiled into the binary.

It contains python-entrypoint-helper.ts (the BUILTIN `pyodide:` module) and everything under
internal/ and types/ (INTERNAL `pyodide-internal:` modules). The per-version Pyodide bundle is
built separately, by bundle/python_bundles.bzl.
"""

load("//:build/wd_ts_bundle.bzl", "wd_ts_bundle")

def pyodide_static():
    internal_data_modules = native.glob([
        "internal/*.py",
        "internal/workers-api/src/*.py",
        "internal/workers-api/src/workers/*.py",
        "internal/topLevelEntropy/*.py",
    ])
    internal_modules = native.glob(
        [
            "internal/*.ts",
            "internal/topLevelEntropy/*.ts",
            "types/*.ts",
            "types/*/*.ts",
        ],
    )
    modules = ["python-entrypoint-helper.ts"]

    wd_ts_bundle(
        name = "pyodide_static",
        eslintrc_json = "eslint.config.mjs",
        import_name = "pyodide",
        internal_data_modules = internal_data_modules,
        internal_modules = internal_modules,
        lint = True,
        modules = modules,
        schema_id = "0xdc8d02dfbdf14025",
        tsconfig_json = "tsconfig.json",
    )
