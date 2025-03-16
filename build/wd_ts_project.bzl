load("@aspect_rules_ts//ts:defs.bzl", "ts_project")
load("@workerd//:build/eslint_test.bzl", "eslint_test")

def wd_ts_project(name, srcs, deps, eslintrc_json = None, testonly = False):
    """Bazel rule for a workerd TypeScript project, setting common options"""

    tsconfig = "//types:tsconfig.json"

    ts_project(
        name = name,
        srcs = srcs,
        deps = deps,
        tsconfig = tsconfig,
        allow_js = True,
        composite = True,
        source_map = True,
        testonly = testonly,
    )

    if eslintrc_json:
        eslint_test(
            name = name,
            eslintrc_json = eslintrc_json,
            tsconfig_json = tsconfig,
            srcs = srcs + deps,
        )
