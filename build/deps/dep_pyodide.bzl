load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive", "http_file")
load("//:build/python_metadata.bzl", "BUNDLE_VERSION_INFO", "PYODIDE_VERSIONS", "PYTHON_LOCKFILES")

def _pyodide_core(*, version, sha256, **_kwds):
    # Use @workerd prefix on build_file so we can use this from edgeworker too
    name = "pyodide-%s" % version
    http_archive(
        name = name,
        build_file = "@workerd//:build/BUILD.pyodide",
        sha256 = sha256,
        urls = ["https://github.com/pyodide/pyodide/releases/download/%s/pyodide-core-%s.tar.bz2" % (version, version)],
    )
    return [name]

# Base URL that the runtime downloads Python packages from at request time. Keep in sync with
# PYTHON_PACKAGES_URL in src/workerd/api/pyodide/pyodide.h.
PYTHON_PACKAGES_URL = "https://pyodide-capnp-bin.edgeworker.net/"

def _stdlib_wheels_repo_impl(rctx):
    # Built-in Python package support has been removed, so workers can no longer request arbitrary
    # packages. The checked-in lock files (src/pyodide/python-lock/) are pre-filtered to contain
    # exactly the packages that are still loaded at runtime (the CPython stdlib modules and the
    # shared libraries they depend on). We download just those wheels and expose them as the package
    # disk cache used by the Python tests, rather than the full upstream all_wheels.zip archive.
    lock = json.decode(rctx.read(rctx.attr.lockfile))
    for pkg in lock["packages"].values():
        file_name = pkg["file_name"]
        rctx.download(
            url = "%spython-package-bucket/%s/%s" % (PYTHON_PACKAGES_URL, rctx.attr.tag, file_name),
            output = file_name,
            sha256 = pkg["sha256"],
        )
    rctx.file("BUILD.bazel", """\
filegroup(
    name = "whls",
    srcs = glob(["*"], exclude = ["BUILD.bazel"]),
    visibility = ["//visibility:public"],
)
""")

_stdlib_wheels_repo = repository_rule(
    implementation = _stdlib_wheels_repo_impl,
    attrs = {
        "tag": attr.string(mandatory = True),
        "lockfile": attr.label(mandatory = True, allow_single_file = True),
    },
)

def _pyodide_packages(*, tag, **_kwds):
    name = "all_pyodide_wheels_%s" % tag
    _stdlib_wheels_repo(
        name = name,
        tag = tag,
        lockfile = "@workerd//src/pyodide:python-lock/pyodide-lock_%s.json" % tag,
    )
    return [name]

VENDOR_R2 = "https://pub-25a5b2f2f1b84655b185a505c7a3ad23.r2.dev/"

def _py_vendor_test_deps(version, name, sha256, abi, **_kwds):
    pyver = "-" + abi.replace(".", "") if abi else ""
    archive_name = name + "_src_" + version
    http_archive(
        name = archive_name,
        build_file_content = """
filegroup(
    name = "all_srcs",
    srcs = glob(["**"]),
    visibility = ["//visibility:public"],
)
""",
        sha256 = sha256,
        url = VENDOR_R2 + name + pyver + "-vendored-for-ew-testing.zip",
    )
    return [archive_name]

PYODIDE_CAPN_BIN = "https://pyodide-capnp-bin.edgeworker.net/"

def _capnp_bundle(id = None, integrity = None, **_kwds):
    if not id or not integrity:
        return []
    name = "pyodide_%s.capnp.bin" % id
    http_file(
        name = name,
        integrity = integrity,
        url = PYODIDE_CAPN_BIN + name,
    )
    return [name]

def _snapshot_http_file(bundle_name, folder, snapshot, integrity, hash, r2_base = PYODIDE_CAPN_BIN):
    if not snapshot:
        return []
    if not integrity:
        fail("Snapshot %s from bundle %s has missing integrity" % (snapshot, bundle_name))
    if folder == "baseline-snapshot/":
        key = hash
    else:
        key = snapshot
    return [struct(
        name = "pyodide-snapshot-" + snapshot,
        snapshot = snapshot,
        integrity = integrity,
        url = r2_base + folder + key,
    )]

def _snapshot_http_files_version(
        name,
        baseline_snapshot = None,
        baseline_snapshot_hash = None,
        baseline_snapshot_integrity = None,
        dedicated_fastapi_snapshot = None,
        dedicated_fastapi_snapshot_integrity = None,
        **_kwds):
    return (_snapshot_http_file(name, "baseline-snapshot/", baseline_snapshot, baseline_snapshot_integrity, baseline_snapshot_hash) +
            _snapshot_http_file(name, "", dedicated_fastapi_snapshot, dedicated_fastapi_snapshot_integrity, None, VENDOR_R2))

def _snapshot_http_files():
    files = []
    for ver in BUNDLE_VERSION_INFO.values():
        files += _snapshot_http_files_version(**ver)

    # Deduplicate generated http_file rules.
    http_files = {info.snapshot: info for info in files}
    deps = []
    for info in http_files.values():
        http_file(
            name = info.name,
            integrity = info.integrity,
            url = info.url,
        )
        deps.append(info.name)
    return deps

def dep_pyodide():
    deps = []
    for info in PYODIDE_VERSIONS:
        deps += _pyodide_core(**info)

    for info in BUNDLE_VERSION_INFO.values():
        deps += _capnp_bundle(**info)
        for pkg in info["vendored_packages_for_tests"].values():
            deps += _py_vendor_test_deps(version = info["name"], **pkg)

    for info in PYTHON_LOCKFILES:
        deps += _pyodide_packages(**info)

    deps += _snapshot_http_files()
    return deps

def _impl(module_ctx):
    deps = dep_pyodide()
    return module_ctx.extension_metadata(
        root_module_direct_deps = deps,
        root_module_direct_dev_deps = [],
    )

pyodide = module_extension(implementation = _impl)
