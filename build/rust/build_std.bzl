# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

"""Builds an instrumented Rust standard library for a rules_rust toolchain.

rules_rust consumes prebuilt standard-library rlibs but does not implement Cargo's
-Zbuild-std. This rule invokes Cargo as a Bazel action instead of reproducing
Rust's internal crate graph as Bazel targets (which would introduce a toolchain
cycle).
"""

load("@rules_rust//rust:toolchain.bzl", "rust_stdlib_filegroup")

# rustc_lib contains host compiler libraries plus prebuilt target metadata. A
# replacement toolchain must discard that metadata while retaining the runtime
# archive needed when rustc links sanitizer-instrumented execution tools.
def _compiler_runtime_files_impl(ctx):
    sanitizer_runtime = "_rt.{}.a".format(ctx.attr.sanitizer)
    return [DefaultInfo(files = depset([
        file
        for file in ctx.files.src
        if "/lib/rustlib/" not in file.path or file.basename.endswith(sanitizer_runtime)
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

def _rust_build_std_impl(ctx):
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
        ] + _STDLIB_CRATES,
        inputs = depset(
            ctx.files.rust_src + ctx.files.rust_toolchain_files,
        ),
        outputs = outputs,
        tools = [ctx.executable.cargo, ctx.executable.rustc],
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
        "sanitizer": attr.string(mandatory = True),
        "target_triple": attr.string(mandatory = True),
        "_build_std": attr.label(
            default = Label("//build/rust:build_std.sh"),
            executable = True,
            allow_single_file = True,
            cfg = "exec",
        ),
    },
)

def instrumented_rust_std(
        name,
        sanitizer,
        target_triple,
        rust_tools,
        rust_src,
        tags = None,
        target_compatible_with = None,
        visibility = None):
    """Defines a rules_rust standard library instrumented by `sanitizer`."""
    rust_build_std(
        name = name + "_build",
        cargo = rust_tools + "//:cargo",
        rustc = rust_tools + "//:rustc",
        rust_src = rust_src + "//:rust_src",
        rust_src_manifest = rust_src + "//:library/Cargo.toml",
        rust_toolchain_files = [rust_tools + "//:rustc_lib"],
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
