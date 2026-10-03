"""Bazel-owned compiler-plugin fixture discovery, compilation, and test grouping."""

load("@rules_rust//rust:defs.bzl", "rust_common")
load("@rules_rust//rust/private:providers.bzl", "DepInfo")
load("@rules_rust//rust/private:utils.bzl", "find_toolchain")

def _fixture_impl(ctx):
    toolchain = find_toolchain(ctx)
    if toolchain.channel != "nightly" or toolchain.exec_triple.str != "x86_64-unknown-linux-gnu" or toolchain.target_triple.str != toolchain.exec_triple.str:
        fail("Dylint fixtures require Linux x86_64 and --config=dylint")

    library = ctx.actions.declare_file(ctx.label.name + "/libworkerd_lints@nightly-" + toolchain.iso_date + "-" + toolchain.exec_triple.str + ".so")
    ctx.actions.symlink(output = library, target_file = ctx.file.library)
    cxx = ctx.attr.cxx[rust_common.crate_info]
    cxx_deps = ctx.attr.cxx[DepInfo]
    dependencies = depset(
        [cxx.metadata or cxx.output],
        transitive = [cxx_deps.transitive_metadata_outputs, cxx_deps.transitive_crate_outputs],
    )
    marker = ctx.actions.declare_file(ctx.label.name + ".ok")
    args = ctx.actions.args()
    args.add(ctx.executable.driver)
    args.add("!failure" if ctx.attr.library_config != "valid" else (ctx.file.expected.path if ctx.file.expected else "-"))
    args.add(marker)
    args.add(ctx.file.source)
    args.add_all([
        "--sysroot",
        toolchain.sysroot,
        "--edition=2024",
        "--crate-type=lib",
        "--crate-name=fixture",
        "--emit=metadata",
        "--error-format=json",
        "-Adead_code",
        # Unpatched Dylint treats invalid/empty library JSON as an empty library list.
        # Unknown lint names must therefore fail, not silently disable enforcement.
        "-Dunknown_lints",
        "-Fexplicit_unsafe_policy",
        "--extern",
        "cxx=" + (cxx.metadata or cxx.output).path,
    ])
    for directory in sorted({f.dirname: True for f in dependencies.to_list()}.keys()):
        args.add_all(["-L", "dependency=" + directory])
    args.add_all(ctx.attr.flags)
    env = {
        "LD_LIBRARY_PATH": ":".join(sorted({f.dirname: True for f in toolchain.rustc_lib.to_list() + toolchain.rust_std.to_list()}.keys())),
        "DYLINT_LIBS": json.encode([library.path]),
    }
    if ctx.attr.library_config == "unset":
        env.pop("DYLINT_LIBS")
    elif ctx.attr.library_config == "malformed":
        env["DYLINT_LIBS"] = "not-json"
    elif ctx.attr.library_config == "empty":
        env["DYLINT_LIBS"] = "[]"
    elif ctx.attr.library_config == "missing":
        env["DYLINT_LIBS"] = json.encode(["/missing/plugin.so"])
    elif ctx.attr.library_config == "nonplugin":
        env["DYLINT_LIBS"] = json.encode([toolchain.rustc.path])

    ctx.actions.run(
        executable = ctx.executable._runner,
        arguments = [args],
        inputs = depset(
            [library, ctx.file.source] + ctx.files.srcs + ([ctx.file.expected] if ctx.file.expected else []),
            transitive = [toolchain.all_files, dependencies, cxx_deps.transitive_proc_macro_data],
        ),
        tools = [ctx.attr.driver[DefaultInfo].files_to_run, ctx.attr._runner[DefaultInfo].files_to_run],
        outputs = [marker],
        env = env,
        mnemonic = "DylintFixture",
        progress_message = "Dylint fixture %{label}",
    )
    executable = ctx.actions.declare_file(ctx.label.name + ".sh")
    ctx.actions.write(executable, """#!/usr/bin/env bash
set -euo pipefail
test -f "$TEST_SRCDIR/$TEST_WORKSPACE/{marker}"
""".format(marker = marker.short_path), is_executable = True)
    return [DefaultInfo(executable = executable, runfiles = ctx.runfiles(files = [marker]))]

rust_lint_fixture_test = rule(
    implementation = _fixture_impl,
    test = True,
    attrs = {
        "driver": attr.label(default = "//tools/rust-lints:driver", executable = True, cfg = "exec"),
        "library": attr.label(default = "//tools/rust-lints:workerd_lints", allow_single_file = True, cfg = "exec"),
        "cxx": attr.label(default = "//src/rust/cxx:cxx", providers = [rust_common.crate_info, DepInfo]),
        "source": attr.label(allow_single_file = [".rs"], mandatory = True),
        "expected": attr.label(allow_single_file = [".out"]),
        "srcs": attr.label_list(allow_files = True),
        "flags": attr.string_list(),
        "library_config": attr.string(default = "valid", values = ["valid", "unset", "malformed", "empty", "missing", "nonplugin"]),
        "_runner": attr.label(default = "//tools/rust-lints:fixture_runner", executable = True, cfg = "exec"),
    },
    toolchains = ["@rules_rust//rust:toolchain_type"],
)

def explicit_unsafe_tests(name, directory):
    """Discover root fixtures and optional .out files; compile both library and test mode."""
    roots = native.glob([directory + "/*.rs"])
    outputs = native.glob([directory + "/*.out"])
    sources = native.glob([directory + "/**/*.rs"])
    expected = {f.removesuffix(".out"): f for f in outputs}
    unknown = [f for f in expected if f + ".rs" not in roots]
    if unknown:
        fail("Expected-output files have no source fixture: " + str(unknown))
    tests = []
    for source in roots:
        stem = source.removesuffix(".rs")
        basename = stem.rsplit("/", 1)[-1]
        for is_test in [False, True]:
            # This fixture proves cfg(test) source is not checked in a library build.
            if (basename == "inactive_test" and is_test) or (basename == "active_test" and not is_test):
                continue
            fixture_name = name + "_" + basename + ("_test_mode" if is_test else "_library")
            flags = ["--cfg=policy_active"]
            if is_test:
                flags.append("--test")
            rust_lint_fixture_test(
                name = fixture_name,
                source = source,
                expected = expected.get(stem),
                srcs = sources,
                flags = flags,
                size = "small",
                tags = ["manual"],
            )
            tests.append(":" + fixture_name)
    native.test_suite(name = name, tests = tests, tags = ["manual"])

def dylint_loader_tests(name, source):
    """Each invalid library configuration must fail to compile a valid crate."""
    tests = []
    for mode in ["unset", "malformed", "empty", "missing", "nonplugin"]:
        fixture_name = name + "_" + mode
        rust_lint_fixture_test(
            name = fixture_name,
            source = source,
            library_config = mode,
            size = "small",
            tags = ["manual"],
        )
        tests.append(":" + fixture_name)
    native.test_suite(name = name, tests = tests, tags = ["manual"])
