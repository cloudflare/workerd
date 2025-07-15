load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive", "http_file")
load("//:build/python_metadata.bzl", "PYODIDE_VERSIONS", "PYTHON_LOCKFILES")

def _pyodide_core(*, version, sha256, **_kwds):
    # Use @workerd prefix on build_file so we can use this from edgeworker too
    http_archive(
        name = "pyodide-%s" % version,
        build_file = "@workerd//:build/BUILD.pyodide",
        sha256 = sha256,
        urls = ["https://github.com/pyodide/pyodide/releases/download/%s/pyodide-core-%s.tar.bz2" % (version, version)],
    )

def _pyodide_packages(*, tag, lockfile_hash, all_wheels_hash, **_kwds):
    http_file(
        name = "pyodide-lock_%s.json" % tag,
        sha256 = lockfile_hash,
        url = "https://github.com/cloudflare/pyodide-build-scripts/releases/download/%s/pyodide-lock.json" % tag,
    )

    # Use @workerd prefix on build_file so we can use this from edgeworker too
    http_archive(
        name = "all_pyodide_wheels_%s" % tag,
        build_file = "@workerd//:build/BUILD.all_pyodide_wheels",
        sha256 = all_wheels_hash,
        urls = ["https://github.com/cloudflare/pyodide-build-scripts/releases/download/%s/all_wheels.zip" % tag],
    )

def _py_vendor_test_deps(name, sha256, url):
    http_archive(
        name = name,
        build_file_content = """
filegroup(
    name = "all_srcs",
    srcs = glob(["**"]),
    visibility = ["//visibility:public"],
)
""",
        sha256 = sha256,
        url = url,
    )

def dep_pyodide():
    for info in PYODIDE_VERSIONS:
        _pyodide_core(**info)

    for info in PYTHON_LOCKFILES:
        _pyodide_packages(**info)

    _py_vendor_test_deps(name = "beautifulsoup4_src", sha256 = "5aa09c5f549443969dda260a70e58e3ac8537bd3d29155b307a3d98b36eb70fd", url = "https://pub-25a5b2f2f1b84655b185a505c7a3ad23.r2.dev/beautifulsoup4-vendored-for-ew-testing.zip")
    _py_vendor_test_deps(name = "fastapi_src", sha256 = "5e6e21dbeda7c1eaadb99e6e52aa2ce45325b51e9a417198701e68e0cfd12a4c", url = "https://pub-25a5b2f2f1b84655b185a505c7a3ad23.r2.dev/fastapi-vendored-for-ew-testing.zip")
