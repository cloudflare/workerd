load("@aspect_rules_ts//ts:defs.bzl", "ts_config", "ts_project")
load("@npm//:eslint/package_json.bzl", eslint_bin = "bin")
load("@workerd//:build/wd_js_bundle.bzl", "wd_js_bundle")

def _to_js(file_name):
    if file_name.endswith(".ts"):
        return file_name.removesuffix(".ts") + ".js"
    return file_name

def _to_d_ts(file_name):
    return file_name.removesuffix(".ts") + ".d.ts"

def wd_ts_bundle(
        name,
        import_name,
        schema_id,
        modules,
        internal_modules,
        tsconfig_json,
        eslintrc_json,
        internal_wasm_modules = [],
        internal_data_modules = [],
        lint = True,
        deps = []):
    """Compiles typescript modules and generates api bundle with the result.

    Args:
      name: bundle target name
      import_name: bundle import name. Modules will be accessible under
          "<import_name>:<module_name>" and internal modules under
          "<import_name>-internal:<module_name>".
      schema_id: bundle capnp schema id,
      modules: list of js and ts source files for builtin modules
      internal_modules: list of js, ts, and d.ts source files for internal modules
      tsconfig_json: tsconfig.json label
      eslintrc_json: eslintrc.json label
      internal_wasm_modules: list of wasm source files
      internal_data_modules: list of data source files
      lint: enables/disables source linting
      deps: additional typescript dependencies
    """
    ts_config(
        name = name + "@tsconfig",
        src = tsconfig_json,
    )

    srcs = modules + internal_modules
    ts_srcs = [src for src in srcs if src.endswith(".ts")]
    declarations = [_to_d_ts(src) for src in ts_srcs if not src.endswith(".d.ts")]

    ts_project(
        name = name + "@tsproject",
        srcs = ts_srcs,
        allow_js = True,
        declaration = True,
        tsconfig = name + "@tsconfig",
        deps = deps,
    )

    wd_js_bundle(
        name = name,
        import_name = import_name,
        # builtin modules are accessible under "<import_name>:<module_name>" name
        builtin_modules = [_to_js(m) for m in modules],
        # internal modules are accessible under "<import_name>-internal:<module_name>" name
        # without "internal/" folder prefix.
        internal_modules = [_to_js(m) for m in internal_modules if not m.endswith(".d.ts")],
        internal_wasm_modules = internal_wasm_modules,
        internal_data_modules = internal_data_modules,
        declarations = declarations,
        schema_id = schema_id,
        deps = deps,
    )

    if lint:
        # todo: lint js_srcs too, not just ts_srcs
        eslint_bin.eslint_test(
            name = name + "@eslint",
            args = [
                "--config $(location {})".format(eslintrc_json),
                "--parser-options project:$(location {})".format(tsconfig_json),
                "-f stylish",
                "--report-unused-disable-directives",
            ] + ["$(location " + src + ")" for src in ts_srcs],
            data = srcs + [
                eslintrc_json,
                tsconfig_json,
                "//:node_modules/@typescript-eslint/eslint-plugin",
            ],
            tags = ["lint"],
            target_compatible_with = select({
                "@platforms//os:windows": ["@platforms//:incompatible"],
                "//conditions:default": [],
            }),
        )
