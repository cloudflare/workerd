"""libc++, libc++abi and libunwind, built for the target as one static library.

The Linux toolchain (build/deps/llvm.MODULE.bazel) links this library into every C++ program, in
place of the prebuilt libraries an LLVM installation would have and Chromium's clang package lacks.
Because the toolchain itself depends on the library, the library can't be compiled with the C++
toolchain: this rule runs the toolchain's clang directly, which is also why it is a rule of its own
rather than a cc_library.

Like LLVM's release builds of these libraries, the library is optimized, without debug information,
whatever the build's compilation mode. Under TSan (-fsanitize=thread in --copt) libc++ and
libc++abi are instrumented, so that TSan sees the synchronization inside them, such as shared_ptr's
reference counting.
"""

_COMMON_FLAGS = [
    "-O3",
    "-DNDEBUG",
    "-fPIC",
    "-ffunction-sections",
    "-fdata-sections",
    "-w",
]

_CXX_FLAGS = [
    "-std=c++26",
    "-nostdinc++",
]

def _object_path(ctx, library, src):
    return "_objs/%s/%s/%s.o" % (ctx.label.name, library, src.short_path.split("/src/", 1)[-1])

def _compile(ctx, library, srcs, flags, inputs):
    # The toolchain root holds bin/clang; the sysroot is its filegroup's package.
    toolchain_path = ctx.file.clang.dirname.rpartition("/")[0]
    sysroot = ctx.attr.sysroot.label
    sysroot_path = (sysroot.workspace_root + "/" + sysroot.package).lstrip("/")
    objects = []
    for src in srcs:
        is_cxx = src.extension == "cpp"
        obj = ctx.actions.declare_file(_object_path(ctx, library, src))
        args = ctx.actions.args()
        args.add("--target=" + ctx.attr.target)
        args.add("--sysroot=" + sysroot_path)
        args.add_all(_COMMON_FLAGS)
        if is_cxx:
            args.add_all(_CXX_FLAGS)
            args.add("-isystem", toolchain_path + "/include/c++/v1")
            args.add("-isystem", "%s/include/%s/c++/v1" % (toolchain_path, ctx.attr.target))
        args.add_all(flags)
        if is_cxx:
            args.add_all(ctx.attr.cxx_flags.get(library, []))
        else:
            args.add_all(ctx.attr.c_flags.get(library, []))
        args.add("-c", src)
        args.add("-o", obj)
        ctx.actions.run(
            executable = ctx.executable.clang,
            arguments = [args],
            inputs = depset([src], transitive = [inputs]),
            outputs = [obj],
            mnemonic = "LibcxxCompile",
            progress_message = "Compiling %{input} for libc++",
        )
        objects.append(obj)
    return objects

def _libcxx_runtime_impl(ctx):
    inputs = depset(transitive = [
        depset(ctx.files.compiler_files),
        depset(ctx.files.sysroot),
        depset(ctx.files.hdrs),
    ])
    sanitizer_flags = []
    if "-fsanitize=thread" in ctx.fragments.cpp.copts:
        sanitizer_flags = ["-fsanitize=thread"]

    # The flags' {llvm_project} is the path of the repository the sources are in.
    llvm_project = ctx.attr.libcxx_srcs[0].label.workspace_root
    objects = []
    for library, srcs in [
        ("libcxx", ctx.files.libcxx_srcs),
        ("libcxxabi", ctx.files.libcxxabi_srcs),
        ("libunwind", ctx.files.libunwind_srcs),
    ]:
        flags = [
            flag.replace("{llvm_project}", llvm_project)
            for flag in ctx.attr.flags.get(library, [])
        ]
        if library != "libunwind":
            flags = flags + sanitizer_flags
        objects.extend(_compile(ctx, library, srcs, flags, inputs))

    archive = ctx.actions.declare_file("lib%s.a" % ctx.label.name)
    args = ctx.actions.args()
    args.add("rcsD", archive)
    args.add_all(objects)
    ctx.actions.run(
        executable = ctx.executable.ar,
        arguments = [args],
        inputs = objects,
        outputs = [archive],
        mnemonic = "LibcxxArchive",
        progress_message = "Archiving %{output}",
    )
    return [DefaultInfo(files = depset([archive]))]

libcxx_runtime = rule(
    implementation = _libcxx_runtime_impl,
    doc = "Compiles libc++, libc++abi and libunwind with clang into one static library.",
    attrs = {
        "ar": attr.label(mandatory = True, executable = True, cfg = "exec", allow_single_file = True),
        "c_flags": attr.string_list_dict(doc = "Flags for C sources, by library."),
        "clang": attr.label(
            mandatory = True,
            executable = True,
            cfg = "exec",
            allow_single_file = True,
            doc = "bin/clang of the toolchain root, whose include directory has libc++'s headers.",
        ),
        "compiler_files": attr.label_list(
            allow_files = True,
            doc = "Files clang needs: its resource directory's headers and libc++'s.",
        ),
        "cxx_flags": attr.string_list_dict(doc = "Flags for C++ sources, by library."),
        "flags": attr.string_list_dict(
            doc = "Flags for all sources, by library. {llvm_project} is the sources' repository.",
        ),
        "hdrs": attr.label_list(allow_files = True, doc = "The libraries' private headers."),
        "libcxx_srcs": attr.label_list(allow_files = True),
        "libcxxabi_srcs": attr.label_list(allow_files = True),
        "libunwind_srcs": attr.label_list(allow_files = True),
        "sysroot": attr.label(mandatory = True, doc = "The sysroot's filegroup, at its top."),
        "target": attr.string(mandatory = True, doc = "The target triple."),
    },
    fragments = ["cpp"],
)
