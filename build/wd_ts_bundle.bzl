load("@aspect_rules_ts//ts:defs.bzl", "ts_config", "ts_project")
load("@workerd//:build/wd_js_bundle.bzl", "wd_js_bundle")
load("@npm//:eslint/package_json.bzl", eslint_bin = "bin")

def _to_js(file_name):
    if file_name.endswith(".ts"):
        return file_name.removesuffix(".ts") + ".js"
    return file_name

def _to_d_ts(file_name):
    return file_name.removesuffix(".ts") + ".d.ts"

def _to_name(file_name):
    return file_name.removesuffix(".ts").removesuffix(".js")

def wd_ts_bundle(
        name,
        import_name,
        schema_id,
        modules,
        internal_modules,
        tsconfig_json,
        eslintrc_json,
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
      internal_modules: list of js and ts source files for internal modules
      tsconfig_json: tsconfig.json label
      eslintrc_json: eslintrc.json label
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
        # builtin modules are accessible under "<import_name>:<module_name>" name
        builtin_modules = dict([(_to_js(m), import_name + ":" + _to_name(m)) for m in modules]),
        const_name = import_name + "Bundle",
        include_prefix = import_name,
        # internal modules are accessible under "<import_name>-internal:<module_name>" name
        # without "internal/" folder prefix.
        internal_modules = dict([(
            _to_js(m),
            import_name + "-internal:" + _to_name(m.removeprefix("internal/")),
        ) for m in internal_modules if not m.endswith(".d.ts")]),
        declarations = declarations,
        schema_id = schema_id,
    )

    if lint:
        # todo: lint js_srcs too, not just ts_srcs
        eslint_bin.eslint_test(
            name = name + "@eslint",
            args = [
                "--config $(execpath {})".format(eslintrc_json),
                "--parser-options project:$(execpath {})".format(tsconfig_json),
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
