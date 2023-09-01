load("@npm//:typescript/package_json.bzl", tsc_bin = "bin")
load("//:build/typescript.bzl", "module_name")

def wd_ts_type_test(src, **kwargs):
    """Bazel rule to test TypeScript file type-checks under @cloudflare/workers-types"""
    name = module_name(src)
    tsc_bin.tsc_test(
        name = name,
        args = [
            "--project",
            "$(location //types:test/types/tsconfig.json)",
            "--types",
            "../../definitions/experimental",
        ],
        data = [
            "//types:types",
            "//types:test/types/tsconfig.json",
            src,
        ],
    )
