load("@npm//:eslint/package_json.bzl", eslint_bin = "bin")

def eslint_test(
        name,
        eslintrc_json,
        tsconfig_json,
        srcs,
        data = []):
    js_srcs = [src for src in srcs if src.endswith(".ts") or src.endswith(".mts") or src.endswith(".js") or src.endswith(".mjs")]

    eslint_bin.eslint_test(
        size = "large",
        name = name + "@eslint",
        args = [
            "--config $(location {})".format(eslintrc_json),
            "--report-unused-disable-directives",
        ] + ["$(location " + src + ")" for src in js_srcs],
        data = srcs + data + [
            eslintrc_json,
            tsconfig_json,
            "//tools:base-eslint",
        ],
        tags = ["lint"],
        target_compatible_with = select({
            "@platforms//os:windows": ["@platforms//:incompatible"],
            "//conditions:default": [],
        }),
    )
