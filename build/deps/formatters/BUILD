load("@aspect_bazel_lib//lib:copy_file.bzl", "copy_file")
load("@aspect_rules_lint//format:defs.bzl", "format_multirun")
load("@bazel_skylib//rules:native_binary.bzl", "native_binary")
load("@npm//:prettier/package_json.bzl", prettier = "bin")

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

native_binary(
    name = "clang-format@rule",
    src = select({
        ":linux_amd64": "@clang-format-linux-amd64//file:file",
        ":linux_arm64": "@clang-format-linux-arm64//file:file",
        ":darwin_arm64": "@clang-format-darwin-arm64//file:file",
        "//conditions:default": "@clang-format-linux-amd64//file:file",
    }),
    out = "clang-format",
    tags = ["manual"],
    visibility = ["//visibility:public"],
)

native_binary(
    name = "ruff@rule",
    src = select({
        ":linux_amd64": "@ruff-linux-amd64//:file",
        ":linux_arm64": "@ruff-linux-arm64//:file",
        ":darwin_arm64": "@ruff-darwin-arm64//:file",
        "//conditions:default": "@ruff-linux-amd64//:file",
    }),
    out = "ruff",
    tags = ["manual"],
    visibility = ["//visibility:public"],
)

copy_file(
    name = "buildifier@rule",
    src = "@buildifier_prebuilt//:buildifier",
    out = "buildifier",
)

prettier.prettier_binary(
    name = "prettier@rule",
    # Include this js_library and its dependencies in the runfiles (runtime dependencies)
    data = ["//:prettierrc"],
    # Allow the binary to be run outside bazel
    env = {"BAZEL_BINDIR": "."},
    fixed_args = [
        # `require` statements in the config file will be resolved relative to its location
        # Therefore to make it hermetic, prettier must be pointed at the copy of the config file
        # in the runfiles folder rather than the one in the source folder.
        "--config=\"$$JS_BINARY__RUNFILES\"/$(rlocationpath //:prettierrc)",
        "--log-level=warn",
    ],
)

format_multirun(
    name = "format",
    cc = ":clang-format@rule",
    javascript = ":prettier@rule",
    python = ":ruff@rule",
    starlark = ":buildifier@rule",
    visibility = ["//:__pkg__"],
)

alias(
    name = "run_clang_format",
    actual = ":format_C++_with_clang-format",
)
