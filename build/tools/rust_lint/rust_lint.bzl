"""Provider-driven Dylint checks, separate from rules_rust's Clippy aspect.

The argument/input adapter uses private APIs from the pinned rules_rust revision.
Keep it coupled to that revision and exercise integration tests on upgrades.
"""

load("@bazel_skylib//lib:structs.bzl", "structs")
load("@rules_rust//rust:defs.bzl", "rust_common")
load("@rules_rust//rust/private:providers.bzl", "LintsInfo")
load("@rules_rust//rust/private:rustc.bzl", "collect_deps", "collect_inputs", "construct_arguments")
load("@rules_rust//rust/private:utils.bzl", "determine_output_hash", "find_cc_toolchain", "find_toolchain")

_WORKERD_REPOSITORY = Label("//tools/rust-lints:driver").workspace_name
_BOOTSTRAP_TARGETS = [
    Label("//tools/rust-lints:driver"),
    Label("//tools/rust-lints:workerd_lints"),
]

WorkerdRustLintInfo = provider(fields = {
    "checks": "depset of successful custom-lint action markers",
    "authored_sources": "depset of original authored source Files, before rules_rust staging",
})

def _crate_info(target):
    if rust_common.crate_info in target:
        return target[rust_common.crate_info]
    if rust_common.test_crate_info in target:
        return target[rust_common.test_crate_info].crate
    return None

def _is_authored_file(file):
    return file.is_source and file.owner.workspace_name == _WORKERD_REPOSITORY

def _authored_sources(ctx):
    # Wrapped tests inherit their production crate's authored source inventory;
    # test-only roots and additional test-only sources do not enter the manifest.
    test_only = getattr(ctx.rule.attr, "testonly", False)
    sources = [] if test_only else [f for f in ctx.rule.files.srcs + ctx.rule.files.compile_data if _is_authored_file(f)]
    if not test_only and hasattr(ctx.rule.file, "crate_root") and ctx.rule.file.crate_root and _is_authored_file(ctx.rule.file.crate_root):
        sources.append(ctx.rule.file.crate_root)
    crate = getattr(ctx.rule.attr, "crate", None)
    inherited = [crate[WorkerdRustLintInfo].authored_sources] if crate and WorkerdRustLintInfo in crate else []
    return depset(sources, transitive = inherited)

def rust_lint_aspect_impl(target, ctx, fixture = False, check = True):
    """Register one matched-nightly compiler action and propagate dependency checks.

    The fixture aspect uses the identical adapter, comparing JSON errors rather
    than publishing compiler metadata. It can propagate providers without
    checking non-fixture dependencies. The ordinary aspect checks all authored
    in-tree sources and never accepts failed lint actions.
    """
    transitive = []
    for name in ["deps", "proc_macro_deps", "crate", "link_deps"]:
        value = getattr(ctx.rule.attr, name, [])
        dependencies = value if type(value) == "list" else ([value] if value else [])
        transitive.extend([dep[WorkerdRustLintInfo].checks for dep in dependencies if WorkerdRustLintInfo in dep])

    crate = _crate_info(target)
    authored = _authored_sources(ctx) if crate else depset()
    markers = []
    if check and crate and target.label.workspace_name == _WORKERD_REPOSITORY and target.label not in _BOOTSTRAP_TARGETS:
        _require_supported_toolchain(ctx)
        originals = {f.short_path: True for f in authored.to_list()}

        # transform_sources stages authored files alongside generated ones. Match
        # their short paths against original source Files, not File.is_source on
        # the staged view. Included compile_data fragments are also authored input.
        paths = sorted({f.path: True for f in crate.srcs.to_list() + crate.compile_data.to_list() + [crate.root] if f.short_path in originals}.keys())
        if paths:
            markers.append(_lint_action(ctx, crate, paths, fixture))

    checks = depset(markers, transitive = transitive)
    return [
        WorkerdRustLintInfo(checks = checks, authored_sources = authored),
        OutputGroupInfo(workerd_rust_lint_checks = checks),
    ]

def _require_supported_toolchain(ctx):
    toolchain = find_toolchain(ctx)
    if toolchain.channel != "nightly" or toolchain.exec_triple.str != "x86_64-unknown-linux-gnu" or toolchain.target_triple.str != toolchain.exec_triple.str:
        fail("Custom Rust lints require Linux x86_64 host/target and --config=dylint")

def _lint_action(ctx, crate, paths, fixture):
    toolchain = find_toolchain(ctx)
    cc_toolchain, features = find_cc_toolchain(ctx)
    dep_info, build_info, _ = collect_deps(
        deps = crate.deps.to_list(),
        proc_macro_deps = crate.proc_macro_deps.to_list(),
        aliases = crate.aliases,
        extra_named_deps = crate.extra_named_deps,
    )
    lint_flags = []
    lint_files = []
    if getattr(ctx.rule.attr, "lint_config", None):
        lints = ctx.rule.attr.lint_config[LintsInfo]
        lint_flags = lints.rustc_lint_flags
        lint_files = lints.rustc_lint_files
    inputs, out_dir, env_files, flag_files, linkstamps, ambiguous_libs = collect_inputs(
        ctx,
        ctx.rule.file,
        ctx.rule.files,
        depset(),
        toolchain,
        cc_toolchain,
        features,
        crate,
        dep_info,
        build_info,
        lint_files,
    )
    marker = ctx.actions.declare_file(ctx.label.name + ".workerd-rust-lint.ok", sibling = crate.output)
    manifest = ctx.actions.declare_file(ctx.label.name + ".workerd-rust-lint.sources", sibling = crate.output)
    ctx.actions.write(manifest, "\n".join(paths) + "\n")
    library = ctx.actions.declare_file(ctx.label.name + ".workerd-rust-lint/libworkerd_lints@nightly-" + toolchain.iso_date + "-" + toolchain.exec_triple.str + ".so")
    ctx.actions.symlink(output = library, target_file = ctx.file._library)
    metadata = None if fixture else ctx.actions.declare_file(ctx.label.name + ".workerd-rust-lint.rmeta", sibling = crate.output)

    # Never reuse the production compiler's diagnostic or metadata outputs.
    crate_fields = structs.to_dict(crate)
    crate_fields.update(rustc_output = None, rustc_rmeta_output = None)
    args, env = construct_arguments(
        ctx = ctx,
        attr = ctx.rule.attr,
        file = ctx.rule.file,
        toolchain = toolchain,
        tool_file = ctx.executable._driver,
        cc_toolchain = cc_toolchain,
        feature_configuration = features,
        crate_info = rust_common.create_crate_info(**crate_fields),
        dep_info = dep_info,
        linkstamp_outs = linkstamps,
        ambiguous_libs = ambiguous_libs,
        output_hash = determine_output_hash(crate.root, ctx.label),
        rust_flags = lint_flags,
        out_dir = out_dir,
        build_env_files = env_files,
        build_flags_files = flag_files,
        emit = ["metadata"] if fixture else [("metadata", metadata)],
        skip_expanding_rustc_env = True,
        error_format = "json" if fixture else "human",
    )
    if crate.is_test:
        args.rustc_flags.add("--test")

    # Applied only to the custom driver; Clippy flags/configuration are unrelated.
    args.rustc_flags.add_all(["-Dunknown_lints", "-Fexplicit_unsafe_policy"])
    env.update({
        "DYLINT_LIBS": json.encode([library.path]),
        "WORKERD_RUST_LINT_SOURCES": manifest.path,
        "LD_LIBRARY_PATH": ":".join(sorted({f.dirname: True for f in toolchain.rustc_lib.to_list() + toolchain.rust_std.to_list()}.keys())),
    })
    tools = [ctx.attr._driver[DefaultInfo].files_to_run, ctx.attr._process_wrapper[DefaultInfo].files_to_run]
    extra_inputs = [library, ctx.file._library, manifest]
    if fixture:
        expected = [f for f in ctx.rule.files.compile_data if f.extension == "out"]
        if len(expected) > 1:
            fail("Integration fixtures accept at most one .out golden file")
        prefix = ctx.actions.args()
        prefix.add(ctx.executable._process_wrapper)
        prefix.add(expected[0] if expected else "-")
        prefix.add(marker)
        arguments = [prefix] + args.all
        executable = ctx.executable._fixture_runner
        tools.append(ctx.attr._fixture_runner[DefaultInfo].files_to_run)
        extra_inputs.extend(expected)
    else:
        args.process_wrapper_flags.add("--touch-file", marker)
        arguments = args.all
        executable = ctx.executable._process_wrapper
    ctx.actions.run(
        executable = executable,
        arguments = arguments,
        inputs = depset(extra_inputs, transitive = [inputs]),
        tools = tools,
        outputs = [marker] + ([metadata] if metadata else []),
        env = env,
        mnemonic = "WorkerdRustLintFixture" if fixture else "WorkerdRustLint",
        progress_message = "Custom Rust lints %{label}",
        # Plugin and manifest paths are explicit env values, not path-mappable argv.
        toolchain = "@rules_rust//rust:toolchain_type",
    )
    return marker

RUST_LINT_ASPECT_ATTRS = {
    "_driver": attr.label(default = "//tools/rust-lints:driver", executable = True, cfg = "exec"),
    "_library": attr.label(default = "//tools/rust-lints:workerd_lints", allow_single_file = True, cfg = "exec"),
    "_process_wrapper": attr.label(default = "@rules_rust//util/process_wrapper", executable = True, cfg = "exec"),
    "_extra_rustc_flag": attr.label(default = "@rules_rust//rust/settings:extra_rustc_flag"),
    "_extra_rustc_flags": attr.label(default = "@rules_rust//rust/settings:extra_rustc_flags"),
    "_extra_exec_rustc_flag": attr.label(default = "@rules_rust//rust/settings:extra_exec_rustc_flag"),
    "_extra_exec_rustc_flags": attr.label(default = "@rules_rust//rust/settings:extra_exec_rustc_flags"),
    "_extra_rustc_env": attr.label(default = "@rules_rust//rust/settings:extra_rustc_env"),
    "_extra_exec_rustc_env": attr.label(default = "@rules_rust//rust/settings:extra_exec_rustc_env"),
    "_per_crate_rustc_flag": attr.label(default = "@rules_rust//rust/settings:per_crate_rustc_flag"),
}

RUST_LINT_TOOLCHAINS = [
    "@rules_rust//rust:toolchain_type",
    config_common.toolchain_type("@bazel_tools//tools/cpp:toolchain_type", mandatory = False),
]

rust_lint_aspect = aspect(
    implementation = rust_lint_aspect_impl,
    attr_aspects = ["deps", "proc_macro_deps", "crate", "link_deps"],
    attrs = RUST_LINT_ASPECT_ATTRS,
    fragments = ["cpp"],
    toolchains = RUST_LINT_TOOLCHAINS,
    provides = [WorkerdRustLintInfo],
)
