load("@aspect_rules_ts//ts:defs.bzl", "ts_project")

def wd_ts_project(name, srcs, deps, testonly = False):
    """Bazel rule for a workerd TypeScript project, setting common options"""

    ts_project(
        name = name,
        srcs = srcs,
        deps = deps,
        tsconfig = "//types:tsconfig.json",
        allow_js = True,
        composite = True,
        source_map = True,
        testonly = testonly,
        # Disable workers to avoid issue with multiple targets
        # (https://github.com/aspect-build/rules_ts/issues/128)
        # TODO: try re-enable these on next aspect_rules_ts update
        supports_workers = 0,
    )
