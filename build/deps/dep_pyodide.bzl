load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive", "http_file")
load("//:build/python_metadata.bzl", "BUNDLE_VERSION_INFO", "PYODIDE_VERSIONS", "PYTHON_LOCKFILES")

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

SNAPSHOT_R2 = "https://pub-25a5b2f2f1b84655b185a505c7a3ad23.r2.dev/"

def _py_vendor_test_deps(version, name, sha256, abi, **_kwds):
    pyver = "-" + abi.replace(".", "") if abi else ""
    http_archive(
        name = name + "_src_" + version,
        build_file_content = """
filegroup(
    name = "all_srcs",
    srcs = glob(["**"]),
    visibility = ["//visibility:public"],
)
""",
        sha256 = sha256,
        url = SNAPSHOT_R2 + name + pyver + "-vendored-for-ew-testing.zip",
    )

def _snapshot_http_file(bundle_name, snapshot, integrity):
    if not snapshot:
        return
    if not integrity:
        fail("Snapshot %s from bundle %s has missing integrity" % (snapshot, bundle_name))
    http_file(
        name = "pyodide-snapshot-" + snapshot,
        integrity = integrity,
        url = SNAPSHOT_R2 + snapshot,
    )

def _snapshot_http_files(
        name,
        baseline_snapshot = None,
        baseline_snapshot_integrity = None,
        numpy_snapshot = None,
        numpy_snapshot_integrity = None,
        fastapi_snapshot = None,
        fastapi_snapshot_integrity = None,
        **_kwds):
    _snapshot_http_file(name, baseline_snapshot, baseline_snapshot_integrity)
    _snapshot_http_file(name, numpy_snapshot, numpy_snapshot_integrity)
    _snapshot_http_file(name, fastapi_snapshot, fastapi_snapshot_integrity)

def dep_pyodide():
    for info in PYODIDE_VERSIONS:
        _pyodide_core(**info)

    for info in BUNDLE_VERSION_INFO.values():
        for pkg in info["vendored_packages_for_tests"].values():
            _py_vendor_test_deps(version = info["name"], **pkg)

    for info in PYTHON_LOCKFILES:
        _pyodide_packages(**info)

    for ver in BUNDLE_VERSION_INFO.values():
        _snapshot_http_files(**ver)
