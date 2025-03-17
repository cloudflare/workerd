load("@aspect_rules_ts//ts:defs.bzl", "ts_config", "ts_project")
load("@workerd//:build/eslint_test.bzl", "eslint_test")
load("@workerd//:build/wd_js_bundle.bzl", "wd_js_bundle")

def to_js(filenames):
    return [_to_js(f) for f in filenames]

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
        internal_json_modules = [],
        lint = True,
        deps = [],
        js_deps = [],
        gen_compile_cache = False):
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
      internal_json_modules: list of json source files
      lint: enables/disables source linting
      deps: additional typescript dependencies
      gen_compile_cache: generate compilation cache of every file and include into the bundle
      js_deps: javascript dependencies
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
        visibility = ["//visibility:public"],
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
        internal_json_modules = internal_json_modules,
        declarations = declarations,
        schema_id = schema_id,
        deps = deps + js_deps,
        gen_compile_cache = gen_compile_cache,
    )

    if lint:
        eslint_test(
            name = name,
            eslintrc_json = eslintrc_json,
            tsconfig_json = tsconfig_json,
            srcs = srcs,
        )
