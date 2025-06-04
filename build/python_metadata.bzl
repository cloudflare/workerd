load("//:build/python/packages_20240829_4.bzl", "PACKAGES_20240829_4")
load("//:build/python/packages_20250324_1.bzl", "PACKAGES_20250324_1")

PYODIDE_VERSIONS = [
    {
        "version": "0.26.0a2",
        "sha256": "fbda450a64093a8d246c872bb901ee172a57fe594c9f35bba61f36807c73300d",
    },
    {
        "version": "0.27.5",
        "sha256": "2e16b053eaa0b1f5761e027e6fc54003567a34e8327bba9a918407accaa4d7c8",
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
PYTHON_LOCKFILES = [meta["info"] for meta in _package_lockfiles]

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
        entry["feature_flags"] = [entry["flag"]]
        result[name] = entry
    dev = result["development"]

    # Uncomment to test with development = 0.27.5
    # dev["real_pyodide_version"] = "0.27.5"
    result["development"] = result[dev["real_pyodide_version"]] | dev
    return result

BUNDLE_VERSION_INFO = make_bundle_version_info([
    {
        "name": "0.26.0a2",
        "pyodide_version": "0.26.0a2",
        "pyodide_date": "2024-03-01",
        "packages": "20240829.4",
        "backport": "59",
        "integrity": "sha256-WPeyKwddTIsG33hWCVCcb3In3BHd1b9TlQbp2g+Q8Kc=",
        "flag": "pythonWorkers",
        "emscripten_version": "3.1.52",
        "python_version": "3.12.1",
        "baseline_snapshot": "baseline-d13ce2f4a.bin",
        "baseline_snapshot_integrity": "sha256-0Tzi9KCt4uCQR7Rph02s9NBx7TVY/sTCb40Lmdlfd7U=",
        "baseline_snapshot_hash": "d13ce2f4a0ade2e09047b469874dacf4d071ed3558fec4c26f8d0b99d95f77b5",
    },
    {
        "name": "0.27.5",
        "pyodide_version": "0.27.5",
        "pyodide_date": "2025-01-16",
        "packages": "20250324.1",
        "backport": "27",
        "integrity": "sha256-0DOMRRWGt67ZuvDKINiyfZyDz7yzDoUd2Vcug5Fhv7Y=",
        "flag": "pythonWorkers20250116",
        "emscripten_version": "3.1.58",
        "python_version": "3.12.7",
        "baseline_snapshot": "baseline-cb0651452.bin",
        "baseline_snapshot_integrity": "sha256-fckrUGeHN443uCivfJC11F924K8g9HAy8RtyaGHmzW8=",
        "baseline_snapshot_hash": "TODO",
    },
    {
        "real_pyodide_version": "0.26.0a2",
        "name": "development",
        "pyodide_version": "dev",
        "pyodide_date": "dev",
        "id": "dev",
        "flag": "pythonWorkersDevPyodide",
        "baseline_snapshot_hash": "92859211804cd350f9e14010afad86e584bdd017dc7acfd94709a87f3220afae",
    },
])
