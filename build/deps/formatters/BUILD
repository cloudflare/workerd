load("@aspect_rules_lint//format:defs.bzl", "format_multirun")
load("@npm//:prettier/package_json.bzl", prettier = "bin")

prettier.prettier_binary(
    name = "prettier",
    # Include this js_library and its dependencies in the runfiles (runtime dependencies)
    data = ["//:prettierrc"],
    # Allow the binary to be run outside bazel
    env = {"BAZEL_BINDIR": "."},
    fixed_args = [
        # `require` statements in the config file will be resolved relative to its location
        # Therefore to make it hermetic, prettier must be pointed at the copy of the config file
        # in the runfiles folder rather than the one in the source folder.
        "--config=\"$$JS_BINARY__RUNFILES\"/$(rlocationpath //:prettierrc)",
        # default log level is "log" which spams on success
        # https://prettier.io/docs/en/cli.html#--log-level
        # NB: prettier 2 names this loglevel, in prettier 3 it's renamed log-level, see
        # https://prettier.io/blog/2023/07/05/3.0.0.html#cli-1
        "--loglevel=warn",
    ],
)

format_multirun(
    name = "format",
    c = "@llvm_toolchain_llvm//:bin/clang-format",
    cc = "@llvm_toolchain_llvm//:bin/clang-format",
    javascript = ":prettier",
    python = "@aspect_rules_lint//format:ruff",
    starlark = "@buildifier_prebuilt//:buildifier",
    visibility = ["//:__pkg__"],
)
