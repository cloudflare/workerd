load("@aspect_rules_js//js:defs.bzl", "js_test")
load("//:build/wd_ts_project.bzl", "wd_ts_project")
load("//:build/typescript.bzl", "js_name", "module_name")

def wd_ts_test(src, deps = [], **kwargs):
    """Bazel rule to compile and run a TypeScript test"""

    name = module_name(src)

    wd_ts_project(
        name = name + "@compile",
        srcs = [src],
        deps = deps,
        testonly = True,
    )

    js_test(
        name = name,
        entry_point = js_name(src),
        data = deps + [name + "@compile"],
        tags = ["no-arm64", "js-test"],
        **kwargs
    )
