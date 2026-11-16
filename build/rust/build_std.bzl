# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

"""Builds an instrumented Rust standard library for a rules_rust toolchain.

rules_rust consumes prebuilt standard-library rlibs but does not implement Cargo's
-Zbuild-std. This rule invokes Cargo as a Bazel action instead of reproducing
Rust's internal crate graph as Bazel targets (which would introduce a toolchain
cycle).
"""

load("@rules_cc//cc:action_names.bzl", "ACTION_NAMES")
load("@rules_cc//cc:find_cc_toolchain.bzl", "find_cc_toolchain", "use_cc_toolchain")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_rust//rust:toolchain.bzl", "rust_stdlib_filegroup")

# rustc_lib contains host compiler libraries plus prebuilt target metadata. A
# replacement toolchain must discard that metadata while retaining the runtime
# library needed when rustc links sanitizer-instrumented execution tools. The
# runtime is a static archive on Linux (librustc-nightly_rt.tsan.a) and a dylib
# on macOS (librustc-nightly_rt.tsan.dylib), so match on the stem only.
def _compiler_runtime_files_impl(ctx):
    sanitizer_runtime = "_rt.{}.".format(ctx.attr.sanitizer)
    return [DefaultInfo(files = depset([
        file
        for file in ctx.files.src
        if "/lib/rustlib/" not in file.path or sanitizer_runtime in file.basename
    ]))]

compiler_runtime_files = rule(
    implementation = _compiler_runtime_files_impl,
    attrs = {
        "sanitizer": attr.string(mandatory = True),
        "src": attr.label(mandatory = True, allow_files = True),
    },
)

# Bazel must know the output names before Cargo assigns metadata hashes. Keep the
# expected -Zbuild-std closure in one place and copy each artifact to a stable name.
_STDLIB_CRATES = [
    "addr2line",
    "adler2",
    "alloc",
    "cfg_if",
    "compiler_builtins",
    "core",
    "getopts",
    "gimli",
    "hashbrown",
    "libc",
    "memchr",
    "miniz_oxide",
    "object",
    "panic_abort",
    "panic_unwind",
    "proc_macro",
    "rustc_demangle",
    "rustc_literal_escaper",
    "rustc_std_workspace_alloc",
    "rustc_std_workspace_core",
    "rustc_std_workspace_std",
    "std",
    "std_detect",
    "test",
    "unwind",
]

def _shell_quote(s):
    return "'" + s.replace("'", "'\\''") + "'"

# Cargo links the standard library's build scripts, which run on the execution platform. They link
# as Bazel links C++ executables, with the C++ toolchain's driver and flags, so the action needs no
# host C compiler. Cargo runs the linker from each crate's directory, while the toolchain's paths
# are relative to the execution root, which build_std.sh exports. Returns the wrapper and the
# environment the driver needs (on macOS, Xcode's).
def _linker_wrapper(ctx, cc_toolchain):
    feature_configuration = cc_common.configure_features(
        ctx = ctx,
        cc_toolchain = cc_toolchain,
        requested_features = ctx.features,
        unsupported_features = ctx.disabled_features,
    )
    variables = cc_common.create_link_variables(
        feature_configuration = feature_configuration,
        cc_toolchain = cc_toolchain,
    )
    driver = cc_common.get_tool_for_action(
        feature_configuration = feature_configuration,
        action_name = ACTION_NAMES.cpp_link_executable,
    )
    flags = cc_common.get_memory_inefficient_command_line(
        feature_configuration = feature_configuration,
        action_name = ACTION_NAMES.cpp_link_executable,
        variables = variables,
    )
    wrapper = ctx.actions.declare_file(ctx.label.name + "-linker.sh")
    ctx.actions.write(
        output = wrapper,
        content = "#!/usr/bin/env bash\ncd \"$BUILD_STD_EXECROOT\" && exec {} \"$@\"\n".format(
            " ".join([_shell_quote(arg) for arg in [driver] + flags]),
        ),
        is_executable = True,
    )
    env = cc_common.get_environment_variables(
        feature_configuration = feature_configuration,
        action_name = ACTION_NAMES.cpp_link_executable,
        variables = variables,
    )
    return wrapper, env

def _rust_build_std_impl(ctx):
    cc_toolchain = find_cc_toolchain(ctx)
    linker, linker_env = _linker_wrapper(ctx, cc_toolchain)
    outputs = [
        ctx.actions.declare_file("{}/lib{}_{}.rlib".format(ctx.label.name, crate, crate))
        for crate in _STDLIB_CRATES
    ]

    output_dir = outputs[0].dirname
    ctx.actions.run(
        mnemonic = "RustBuildStd",
        progress_message = "Building the Rust standard library with {} instrumentation".format(ctx.attr.sanitizer),
        executable = ctx.executable._build_std,
        arguments = [
            ctx.executable.rustc.dirname.removesuffix("/bin"),
            ctx.file.rust_src_manifest.dirname,
            output_dir + "-target",
            output_dir,
            output_dir + "-work",
            ctx.attr.target_triple,
            ctx.attr.sanitizer,
            " ".join(ctx.attr.rustflags),
            linker.path,
        ] + _STDLIB_CRATES,
        inputs = depset(
            ctx.files.rust_src + ctx.files.rust_toolchain_files,
            transitive = [cc_toolchain.all_files] + [
                files[DefaultInfo].default_runfiles.files
                for files in ctx.attr.rust_toolchain_files
            ],
        ),
        outputs = outputs,
        env = linker_env,
        tools = [ctx.executable.cargo, ctx.executable.rustc, linker],
    )

    return [DefaultInfo(files = depset(outputs))]

rust_build_std = rule(
    implementation = _rust_build_std_impl,
    attrs = {
        "cargo": attr.label(mandatory = True, executable = True, allow_files = True, cfg = "exec"),
        "rustc": attr.label(mandatory = True, executable = True, allow_files = True, cfg = "exec"),
        "rust_src": attr.label(mandatory = True, allow_files = True),
        "rust_src_manifest": attr.label(mandatory = True, allow_single_file = True),
        "rust_toolchain_files": attr.label_list(mandatory = True, allow_files = True),
        "rustflags": attr.string_list(),
        "sanitizer": attr.string(mandatory = True),
        "target_triple": attr.string(mandatory = True),
        "_build_std": attr.label(
            default = Label("//build/rust:build_std.sh"),
            executable = True,
            allow_single_file = True,
            cfg = "exec",
        ),
    },
    fragments = ["cpp"],
    toolchains = use_cc_toolchain(),
)

def instrumented_rust_std(
        name,
        sanitizer,
        target_triple,
        rust_tools,
        rust_src,
        rustflags = [],
        tags = None,
        target_compatible_with = None,
        visibility = None):
    """Defines a rules_rust standard library instrumented by `sanitizer`.

    `rustflags` are extra rustc flags every standard-library crate is compiled with.
    """
    rust_build_std(
        name = name + "_build",
        cargo = rust_tools + "//:cargo",
        rustc = rust_tools + "//:rustc",
        rust_src = rust_src + "//:rust_src",
        rust_src_manifest = rust_src + "//:library/Cargo.toml",
        # rustc's libraries, and the prebuilt standard library and self-contained linker that the
        # build scripts link with.
        rust_toolchain_files = [
            rust_tools + "//:rustc_lib",
            rust_tools + "//:rust-lld",
            rust_tools + "//:rust_std-" + target_triple,
        ],
        rustflags = rustflags,
        sanitizer = sanitizer,
        target_triple = target_triple,
        tags = tags,
        target_compatible_with = target_compatible_with,
    )
    rust_stdlib_filegroup(
        name = name,
        srcs = [":" + name + "_build"],
        tags = tags,
        target_compatible_with = target_compatible_with,
        visibility = visibility,
    )
