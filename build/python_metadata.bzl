load("//:build/python/packages_20240829_4.bzl", "PACKAGES_20240829_4")
load("//:build/python/packages_20250324_1.bzl", "PACKAGES_20250324_1")

# The below is a list of pyodide-lock.json files for each package bundle version that we support.
# Each of these gets embedded in the workerd and EW binary.
#
# The key is the `packages` field in pythonSnapshotRelease and the value is the sha256 checksum of
# the lock file.
PYTHON_LOCKFILES = {
    "20240829.4": "c2d9c67ea55a672b95a3beb8d66bfbe7df736edb4bb657383b263151e7e85ef4",
    "20250324.1": "3e5a9317dc0cfcf63e556034bf0e87b958bd6debcfdccdfffc8ce477cc439626",
}

# This is a dictionary mapping a Python version with its packages version.
#
# NOTE: this needs to be kept in sync with compatibility-date.capnp.
PYTHON_VERSION_TO_PACKAGES = {
    "0.26.0a2": "20240829.4",
    "0.27.1": "20250324.1",
    "development": "20240829.4",
}

# This is a dictionary mapping a `packages` date/version (the `packages` field in
# pythonSnapshotRelease) to a list of package names which are in that packages bundle. The list is
# also in the form of a dictionary and maps to the import names for that package which should be
# tested (as some packages may expose more than one module).
#
# IMPORTANT: packages that are present here should never be removed after the package version is
# released to the public. This is so that we don't break workers using those packages.
#
# ORDER MATTERS: the order of the keys in this dictionary matters, older package bundles should come
# first.
PYTHON_IMPORTS_TO_TEST = {
    "20240829.4": PACKAGES_20240829_4,
    "20250324.1": PACKAGES_20250324_1,
}

# Each new package bundle should contain the same packages as the previous. We verify this
# constraint here.
def verify_no_packages_were_removed():
    package_dates = PYTHON_IMPORTS_TO_TEST.keys()  # Assuming dict order is stable in Skylark.
    for i in range(0, len(package_dates) - 1):
        curr_pkgs = PYTHON_IMPORTS_TO_TEST[package_dates[i]]
        next_pkgs = PYTHON_IMPORTS_TO_TEST[package_dates[i + 1]]
        missing_pkgs = [pkg for pkg in curr_pkgs if pkg not in next_pkgs]
        if missing_pkgs:
            fail(str(missing_pkgs) + " from packages version ", package_dates[i], " not in ", package_dates[i + 1])

verify_no_packages_were_removed()

def make_bundle_version_info(versions):
    result = {}
    for entry in versions:
        name = entry["name"]
        if entry["name"] != "development":
            entry["id"] = entry["pyodide_version"] + "_" + entry["pyodide_date"] + "_" + entry["backport"]
        result[name] = entry
    return result

BUNDLE_VERSION_INFO = make_bundle_version_info([
    {
        "name": "0.26.0a2",
        "pyodide_version": "0.26.0a2",
        "pyodide_date": "2024-03-01",
        "backport": "26",
        "integrity": "sha256-PGbANe9AeoPAf2EO4BCrw0iUox+C7CgJLGR/79r04yM=",
        "feature_flags": [],
        "emscripten_version": "3.1.52",
        "python_version": "3.12.1",
        "baseline_snapshot": "baseline-d13ce2f4a.bin",
    },
    {
        "name": "0.27.1",
        "pyodide_version": "0.27.1",
        "pyodide_date": "2025-01-16",
        "backport": "14",
        "integrity": "sha256-yJduukBGEXPOlWEZkg6gz0j31Kw1AHyy/t9+FYJcagw=",
        "feature_flags": ["pythonWorkers20250116"],
        "emscripten_version": "3.1.58",
        "python_version": "3.12.7",
        "baseline_snapshot": "baseline-700487b8d.bin",
    },
    {
        "name": "development",
        "id": "dev",
        "feature_flags": ["pythonWorkersDevPyodide", "pythonExternalPackages"],
        "emscripten_version": "3.1.52",
        "python_version": "3.12.1",
        "baseline_snapshot": "baseline-d13ce2f4a.bin",
    },
])
