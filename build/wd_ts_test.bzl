load("@aspect_rules_js//js:defs.bzl", "js_test")
load("//:build/typescript.bzl", "js_name", "module_name")
load("//:build/wd_ts_project.bzl", "wd_ts_project")

def wd_ts_test(src, tsconfig_json, deps = [], eslintrc_json = None, composite = False, **kwargs):
    """Bazel rule to compile and run a TypeScript test"""

    name = module_name(src)

    wd_ts_project(
        name = name + "@compile",
        srcs = [src],
        deps = deps,
        testonly = True,
        eslintrc_json = eslintrc_json,
        tsconfig_json = tsconfig_json,
        composite = composite,
    )

    # TODO(soon): Remove 'no-coverage' tag once
    # https://github.com/aspect-build/rules_js/pull/2653 lands.
    # This is a workaround for a bug in aspect_rules_js with the
    # --experimental_split_coverage_postprocessing flag.
    js_test(
        name = name,
        entry_point = js_name(src),
        data = deps + [name + "@compile"],
        tags = ["no-arm64", "js-test", "no-coverage"],
        **kwargs
    )
