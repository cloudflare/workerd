load("@bazel_skylib//rules:copy_file.bzl", "copy_file")

config_setting(
    name = "linux_amd64",
    constraint_values = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
)

config_setting(
    name = "linux_arm64",
    constraint_values = [
        "@platforms//os:linux",
        "@platforms//cpu:arm64",
    ],
)

config_setting(
    name = "darwin_amd64",
    constraint_values = [
        "@platforms//os:macos",
        "@platforms//cpu:x86_64",
    ],
)

config_setting(
    name = "darwin_arm64",
    constraint_values = [
        "@platforms//os:macos",
        "@platforms//cpu:arm64",
    ],
)

config_setting(
    name = "windows_amd64",
    constraint_values = [
        "@platforms//os:windows",
        "@platforms//cpu:x86_64",
    ],
)

copy_file(
    name = "clang-format@rule",
    src = select({
        ":linux_amd64": "@clang-format-linux-amd64//file:file",
        ":linux_arm64": "@clang-format-linux-arm64//file:file",
        ":darwin_arm64": "@clang-format-darwin-arm64//file:file",
        "//conditions:default": "@clang-format-linux-amd64//file:file",
    }),
    out = "clang-format",
    visibility = ["//visibility:public"],
)

copy_file(
    name = "ruff@rule",
    src = select({
        ":linux_amd64": "@ruff-linux-amd64//:file",
        ":linux_arm64": "@ruff-linux-arm64//:file",
        ":darwin_arm64": "@ruff-darwin-arm64//:file",
        "//conditions:default": "@ruff-linux-amd64//:file",
    }),
    out = "ruff",
    visibility = ["//visibility:public"],
)

copy_file(
    name = "buildifier@rule",
    src = select({
        ":linux_amd64": "@buildifier-linux-amd64//file:file",
        ":linux_arm64": "@buildifier-linux-arm64//file:file",
        ":darwin_amd64": "@buildifier-darwin-amd64//file:file",
        ":darwin_arm64": "@buildifier-darwin-arm64//file:file",
        ":windows_amd64": "@buildifier-windows-amd64//file:file",
        "//conditions:default": "@buildifier-linux-amd64//file:file",
    }),
    out = "buildifier",
    visibility = ["//visibility:public"],
)
