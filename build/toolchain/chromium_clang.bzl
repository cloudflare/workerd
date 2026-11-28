"""Chromium's prebuilt clang, as a toolchain root for toolchains_llvm (build/deps/llvm.MODULE.bazel).

Chromium publishes the clang it builds with, for Linux x86_64 hosts, as a small package of the
compiler, lld, a few binutils replacements and clang's runtime libraries for every target it
supports, and its coverage tools as a second package. This rule unpacks both, adds a tool name the
package leaves out, and adds libc++'s headers, taken from the libc++ sources the build compiles
(//build/toolchain:libcxx), so that the root has the layout of an LLVM installation.
"""

# The per-target headers of an LLVM installation, which configure libc++. Their values are those
# of LLVM's own Linux release builds.
_CONFIG_SITE = """\
#ifndef _LIBCPP___CONFIG_SITE
#define _LIBCPP___CONFIG_SITE

#define _LIBCPP_ABI_VERSION 1
#define _LIBCPP_ABI_NAMESPACE __1
#define _LIBCPP_ABI_FORCE_ITANIUM 0
#define _LIBCPP_ABI_FORCE_MICROSOFT 0
#define _LIBCPP_HAS_THREADS 1
#define _LIBCPP_HAS_MONOTONIC_CLOCK 1
#define _LIBCPP_HAS_MUSL_LIBC 0
#define _LIBCPP_HAS_THREAD_API_PTHREAD 0
#define _LIBCPP_HAS_THREAD_API_EXTERNAL 0
#define _LIBCPP_HAS_THREAD_API_WIN32 0
#define _LIBCPP_HAS_THREAD_API_C11 0
#define _LIBCPP_HAS_VENDOR_AVAILABILITY_ANNOTATIONS 0
#define _LIBCPP_HAS_FILESYSTEM 1
#define _LIBCPP_HAS_RANDOM_DEVICE 1
#define _LIBCPP_HAS_LOCALIZATION 1
#define _LIBCPP_HAS_UNICODE 1
#define _LIBCPP_HAS_WIDE_CHARACTERS 1
#define _LIBCPP_HAS_TIME_ZONE_DATABASE 1
#define _LIBCPP_INSTRUMENTED_WITH_ASAN 0
#define _LIBCPP_PSTL_BACKEND_STD_THREAD
// _LIBCPP_HARDENING_MODE_NONE
#define _LIBCPP_HARDENING_MODE_DEFAULT 2
// _LIBCPP_ASSERTION_SEMANTIC_HARDENING_DEPENDENT
#define _LIBCPP_ASSERTION_SEMANTIC_DEFAULT 2
#define _LIBCPP_LIBC_PICOLIBC 0
#define _LIBCPP_LIBC_NEWLIB 0
#define _LIBCPP_LIBC_LLVM_LIBC 0

#endif // _LIBCPP___CONFIG_SITE
"""

_TRIPLES = [
    "aarch64-unknown-linux-gnu",
    "x86_64-unknown-linux-gnu",
]

# A name that the package leaves out and toolchains_llvm runs, as a link to the binary that takes
# its role from the name it is run by.
_TOOL_LINKS = {"clang-cpp": "clang"}

def _chromium_clang_impl(rctx):
    for package in [rctx.attr.clang, rctx.attr.coverage]:
        rctx.download_and_extract(url = package["url"], sha256 = package["sha256"])
    for name, target in _TOOL_LINKS.items():
        rctx.symlink("bin/" + target, "bin/" + name)

    versions = rctx.path("lib/clang").readdir()
    if len(versions) != 1:
        fail("expected one clang resource directory, found %s" % versions)
    version = versions[0].basename

    # Copies, not links: the headers must be files of this repository, under the toolchain root.
    # cp keeps their bytes as they are, which rctx.read and rctx.file would not promise.
    llvm_project = rctx.path(rctx.attr.libcxx_sources).dirname
    include = "include/c++/v1"

    def mkdir(directory):
        rctx.file(directory + "/.keep", "")
        rctx.delete(directory + "/.keep")

    def copy(source, destination):
        result = rctx.execute(["cp", "-R", str(llvm_project) + "/" + source, destination])
        if result.return_code != 0:
            fail("copying %s failed: %s" % (source, result.stderr))

    mkdir(include)
    copy("libcxx/include/.", include)
    for name in ["CMakeLists.txt", "__config_site.in", "module.modulemap.in"]:
        rctx.delete(include + "/" + name)
    copy("libcxxabi/include/cxxabi.h", include)
    copy("libcxxabi/include/__cxxabi_config.h", include)
    for triple in _TRIPLES:
        target_include = "include/%s/c++/v1" % triple
        mkdir(target_include)
        rctx.file(target_include + "/__config_site", _CONFIG_SITE)
        copy("libcxx/vendor/llvm/default_assertion_handler.in", target_include + "/__assertion_handler")

    rctx.template("BUILD.bazel", rctx.attr._build_file, {"{LLVM_VERSION}": version})

    # The packages are pinned by hash, and the headers by the sources' commit.
    return rctx.repo_metadata(reproducible = True)

chromium_clang = repository_rule(
    implementation = _chromium_clang_impl,
    attrs = {
        "clang": attr.string_dict(mandatory = True, doc = "The url and sha256 of the clang package."),
        "coverage": attr.string_dict(
            mandatory = True,
            doc = "The url and sha256 of the llvm-code-coverage package.",
        ),
        "libcxx_sources": attr.label(
            mandatory = True,
            doc = "A file at the top of an llvm-project checkout, whose libc++ the headers are.",
        ),
        "_build_file": attr.label(default = "//build/toolchain:BUILD.chromium_clang"),
    },
)
