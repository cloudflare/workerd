"""A Linux sysroot pared down to the Debian packages that builds compile and link against.

Chromium's sysroots carry the headers and libraries of a desktop (GTK, Qt, X11, Vulkan, Python,
Perl, ...), several times the C library and kernel headers that our builds use. Every C++ and Rust
action takes the sysroot as an input, so a remote executor fetches and stages all of it per action.
This keeps only the files of the named packages, as each package's dpkg file list records them.
"""

def _sysroot_impl(rctx):
    rctx.download_and_extract(
        url = rctx.attr.urls,
        sha256 = rctx.attr.sha256,
        type = "tar.xz",
        output = "sysroot",
    )
    root = rctx.path("sysroot")
    root_prefix = str(root) + "/"
    real_root_prefix = str(root.realpath) + "/"

    # dpkg leaves symlinks out of a package's md5sums, so a symlink is kept when the file it
    # resolves to is. The sysroot's symlinks are all relative, so they resolve within it.
    keep = {}
    for package in rctx.attr.packages:
        md5sums = root.get_child("debian", package, "DEBIAN", "md5sums")
        if not md5sums.exists:
            fail("The sysroot has no package {}".format(package))
        for line in rctx.read(md5sums).splitlines():
            keep[line.split("  ", 1)[1]] = True

    # Starlark has neither recursion nor while loops, so this walks the tree from an explicit stack.
    # A directory is visited after its parent, so walking the visit order backwards reaches children
    # first, which lets directories left empty go too.
    visited = []
    stack = [root]
    for _ in range(1000000):
        if not stack:
            break
        dir = stack.pop()
        visited.append(dir)
        for entry in dir.readdir():
            path = str(entry)[len(root_prefix):]
            if not entry.exists:
                rctx.delete(entry)
                continue
            real = str(entry.realpath)
            if real != real_root_prefix + path:
                # A symlink.
                if not real.startswith(real_root_prefix) or real[len(real_root_prefix):] not in keep:
                    rctx.delete(entry)
            elif entry.is_dir:
                stack.append(entry)
            elif path not in keep:
                rctx.delete(entry)
    for dir in reversed(visited[1:]):
        if not dir.readdir():
            rctx.delete(dir)

    # Source directories are cheaper than per-file groups: Bazel stages one as a single symlink and
    # memoizes it as one Merkle tree node.
    build = """filegroup(
    name = "sysroot",
    srcs = ["."],
    visibility = ["//visibility:public"],
)
"""
    if rctx.attr.cc_includes:
        build = 'load("@rules_cc//cc:cc_library.bzl", "cc_library")\n\n' + build + """
cc_library(
    name = "headers",
    hdrs = glob(["usr/include/**"]),
    includes = {},
    visibility = ["//visibility:public"],
)
""".format(repr(rctx.attr.cc_includes))
    rctx.file("sysroot/BUILD.bazel", build)

    # Not reproducible, so the remote repo contents cache, which keeps a repository's files remote
    # until an action names them, leaves it out: actions take the sysroot as one source directory,
    # which a local action finds only when the whole repository is on disk.
    return rctx.repo_metadata(reproducible = False)

sysroot = repository_rule(
    implementation = _sysroot_impl,
    attrs = {
        "cc_includes": attr.string_list(
            doc = "Directories, relative to the sysroot, that a `headers` cc_library exports.",
        ),
        "packages": attr.string_list(mandatory = True, doc = "The Debian packages to keep."),
        "sha256": attr.string(mandatory = True),
        "urls": attr.string_list(mandatory = True),
    },
)
