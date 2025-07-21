load("@aspect_rules_ts//ts:defs.bzl", "ts_config", "ts_project")
load("@workerd//:build/eslint_test.bzl", "eslint_test")

def wd_ts_project(name, srcs, deps, tsconfig_json, eslintrc_json = None, testonly = False, data = []):
    """Bazel rule for a workerd TypeScript project, setting common options"""

    ts_config(
        name = name + "@tsconfig",
        src = tsconfig_json,
    )

    ts_project(
        name = name,
        srcs = srcs,
        deps = deps,
        tsconfig = ":" + name + "@tsconfig",
        allow_js = True,
        composite = True,
        declaration = True,
        source_map = True,
        testonly = testonly,
        data = data,
    )

    if eslintrc_json:
        eslint_test(
            name = name,
            eslintrc_json = eslintrc_json,
            tsconfig_json = tsconfig_json,
            srcs = srcs + deps,
        )
