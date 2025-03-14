load("@aspect_rules_ts//ts:defs.bzl", "ts_project")
load("@npm//:eslint/package_json.bzl", eslint_bin = "bin")

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
        eslint_bin.eslint_test(
            size = "large",
            name = name + "@eslint",
            args = [
                "--config $(location {})".format(eslintrc_json),
                "--parser-options project:$(location {})".format(tsconfig),
                "-f stylish",
                "--report-unused-disable-directives",
            ] + ["$(location " + src + ")" for src in srcs],
            data = srcs + deps + [
                eslintrc_json,
                tsconfig,
                "//tools:base-eslint",
                "//:prettierrc",
            ],
            tags = ["lint"],
            target_compatible_with = select({
                "@platforms//os:windows": ["@platforms//:incompatible"],
                "//conditions:default": [],
            }),
        )
