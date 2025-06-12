load("@aspect_bazel_lib//lib:base64.bzl", "base64")
load("@aspect_bazel_lib//lib:strings.bzl", "chr")
load("//:build/python/packages_20240829_4.bzl", "PACKAGES_20240829_4")
load("//:build/python/packages_20250606.bzl", "PACKAGES_20250606")

def _chunk(data, length):
    return [data[i:i + length] for i in range(0, len(data), length)]

def hex_to_b64(hex):
    s = ""
    for chunk in _chunk(hex, 2):
        s += chr(int(chunk, 16))
    return "sha256-" + base64.encode(s)

PYODIDE_VERSIONS = [
    {
        "version": "0.26.0a2",
        "sha256": "fbda450a64093a8d246c872bb901ee172a57fe594c9f35bba61f36807c73300d",
    },
    {
        "version": "0.27.7",
        "sha256": "9bc8f127db6c590b191b9aee754022cb41b1a36c7bac233776c11c5ecb541be8",
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
    PACKAGES_20250606,
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
        if "packages" in entry:
            entry["packages"] = entry["packages"]["info"]["tag"]
        if "baseline_snapshot_hash" in entry:
            entry["baseline_snapshot_integrity"] = hex_to_b64(entry["baseline_snapshot_hash"])
        result[name] = entry
    dev = result["development"]

    # Uncomment to test with development = 0.27.7
    # dev["real_pyodide_version"] = "0.27.7"
    result["development"] = result[dev["real_pyodide_version"]] | dev
    return result

BUNDLE_VERSION_INFO = make_bundle_version_info([
    {
        "name": "0.26.0a2",
        "pyodide_version": "0.26.0a2",
        "pyodide_date": "2024-03-01",
        "packages": PACKAGES_20240829_4,
        "backport": "63",
        "integrity": "sha256-xrG65VJvao9GYH07C73Uq2jA9DW7O1DP16fiZo36Xq0=",
        "flag": "pythonWorkers",
        "emscripten_version": "3.1.52",
        "python_version": "3.12.1",
        "baseline_snapshot": "baseline-d13ce2f4a.bin",
        "baseline_snapshot_hash": "d13ce2f4a0ade2e09047b469874dacf4d071ed3558fec4c26f8d0b99d95f77b5",
    },
    {
        "name": "0.27.7",
        "pyodide_version": "0.27.7",
        "pyodide_date": "2025-01-16",
        "packages": PACKAGES_20250606,
        "backport": "2",
        "integrity": "sha256-04qtaf3jr6q7mixWrpeASgYzTW1WHb9NEILBGl8M9hk=",
        "flag": "pythonWorkers20250116",
        "emscripten_version": "3.1.58",
        "python_version": "3.12.7",
        "baseline_snapshot": "baseline-59fa311f4.bin",
        "baseline_snapshot_hash": "59fa311f4af0bb28477e2fa17f54dc254ec7fa6f02617b832b141854e44bd621",
    },
    {
        "real_pyodide_version": "0.26.0a2",
        "name": "development",
        "pyodide_version": "dev",
        "pyodide_date": "dev",
        "id": "dev",
        "flag": "pythonWorkersDevPyodide",
    },
])
