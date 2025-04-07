load("//:build/python/packages_20240829_4.bzl", "PACKAGES_20240829_4")
load("//:build/python/packages_20250324_1.bzl", "PACKAGES_20250324_1")

PYODIDE_VERSIONS = [
    {
        "version": "0.26.0a2",
        "sha256": "fbda450a64093a8d246c872bb901ee172a57fe594c9f35bba61f36807c73300d",
    },
    {
        "version": "0.27.1",
        "sha256": "6e45f93c71ed21bff4a06d6d9d8e27e815269dabf8ab34dc400939fd45edc665",
    },
]

# This is the list of all the package metadata that we use.
#
# IMPORTANT: packages that are present here should never be removed after the package version is
# released to the public. This is so that we don't break workers using those packages.
#
# ORDER MATTERS: the order of the keys in this dictionary matters, older package bundles should come
# first.
_package_lockfiles = [
    PACKAGES_20240829_4,
    PACKAGES_20250324_1,
]

# The below is a list of pyodide-lock.json files for each package bundle version that we support.
# Each of these gets embedded in the workerd and EW binary.
#
# The key is the `packages` field in pythonSnapshotRelease and the value is the sha256 checksum of
# the lock file. Used by both workerd and edgeworker to download the package lockfiles.
PYTHON_LOCKFILES = {meta["info"]["tag"]: meta["info"]["lockfile_hash"] for meta in _package_lockfiles}

# Used to generate the import tests, where we import each top level name from each package and check
# that it doesn't fail.
PYTHON_IMPORTS_TO_TEST = {meta["info"]["tag"]: meta["import_tests"] for meta in _package_lockfiles}

# Each new package bundle should contain the same packages as the previous. We verify this
# constraint here.
def verify_no_packages_were_removed():
    for curr_info, next_info in zip(_package_lockfiles[:-1], _package_lockfiles[1:]):
        curr_pkgs = curr_info["import_tests"]
        next_pkgs = next_info["import_tests"]
        missing_pkgs = [pkg for pkg in curr_pkgs if pkg not in next_pkgs]
        if missing_pkgs:
            fail("Some packages from version ", curr_info["info"]["tag"], " missing in version", next_info["info"]["tag"], ":\n", "   ", ", ".join(missing_pkgs), "\n\n")

verify_no_packages_were_removed()

def _bundle_id(*, pyodide_version, pyodide_date, backport, **_kwds):
    return "%s_%s_%s" % (pyodide_version, pyodide_date, backport)

def make_bundle_version_info(versions):
    result = {}
    for entry in versions:
        name = entry["name"]
        if entry["name"] != "development":
            entry["id"] = _bundle_id(**entry)
        result[name] = entry
    return result

# NOTE: This data needs to be kept in sync with compatibility-date.capnp.
# Particularly the packages and backport fields.
BUNDLE_VERSION_INFO = make_bundle_version_info([
    {
        "name": "0.26.0a2",
        "pyodide_version": "0.26.0a2",
        "pyodide_date": "2024-03-01",
        "packages": "20240829.4",
        "backport": "29",
        "integrity": "sha256-sVWQzDU+mUsVOp481/oCUuDAWEbKKuc4gxToLVezg6g=",
        "feature_flags": [],
        "emscripten_version": "3.1.52",
        "python_version": "3.12.1",
        "baseline_snapshot": "baseline-d13ce2f4a.bin",
    },
    {
        "name": "0.27.1",
        "pyodide_version": "0.27.1",
        "pyodide_date": "2025-01-16",
        "packages": "20250324.1",
        "backport": "17",
        "integrity": "sha256-jV3zgLs1uevw2s9woVGdcRTjkqTWEOg5ToMAWS1mo8w=",
        "feature_flags": ["pythonWorkers20250116"],
        "emscripten_version": "3.1.58",
        "python_version": "3.12.7",
        "baseline_snapshot": "baseline-700487b8d.bin",
    },
    {
        "name": "development",
        "id": "dev",
        "feature_flags": ["pythonWorkersDevPyodide"],
        "emscripten_version": "3.1.52",
        "python_version": "3.12.1",
        "packages": "20240829.4",
        "baseline_snapshot": "baseline-d13ce2f4a.bin",
    },
])
