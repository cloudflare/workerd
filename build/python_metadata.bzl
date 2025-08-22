load("@aspect_bazel_lib//lib:base64.bzl", "base64")
load("@aspect_bazel_lib//lib:strings.bzl", "chr")
load("//:build/python/packages_20240829_4.bzl", "PACKAGES_20240829_4")
load("//:build/python/packages_20250808.bzl", "PACKAGES_20250808")

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
        "version": "0.28.2",
        "sha256": "c9f6dd067d119e50850849f7428e3c636ecbc2684a0d2ff992f3bd48a1062b6c",
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
    PACKAGES_20250808,
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

def _add_integrity(entry):
    for key, value in entry.items():
        if not key.endswith("_hash"):
            continue
        newkey = key.removesuffix("_hash") + "_integrity"
        entry[newkey] = hex_to_b64(value)

def _make_vendored_packages(entry):
    if entry["name"] == "development":
        return
    vendor_tests = {}
    for e in entry["vendored_packages_for_tests"]:
        vendor_tests[e["name"]] = e
    entry["vendored_packages_for_tests"] = vendor_tests

def _check_pyodide_versions(version_info):
    pyodide_versions = {ver["version"]: 1 for ver in PYODIDE_VERSIONS}
    pyodide_versions["dev"] = 1
    for entry in version_info.values():
        if entry["pyodide_version"] not in pyodide_versions:
            fail("Version %s of Pyodide not in PYODIDE_VERSIONS" % entry["pyodide_version"])

def _make_bundle_version_info(versions):
    result = {}
    for entry in versions:
        name = entry["name"]
        if name != "development":
            entry["id"] = _bundle_id(**entry)
        entry["feature_flags"] = [entry["flag"]]
        entry["feature_string_flags"] = [entry["enable_flag_name"]]
        if "packages" in entry:
            entry["packages"] = entry["packages"]["info"]["tag"]
        _add_integrity(entry)
        result[name] = entry
        _make_vendored_packages(entry)

    dev = result["development"]

    # Uncomment to test with development = 0.28.2
    # dev["real_pyodide_version"] = "0.28.2"
    result["development"] = result[dev["real_pyodide_version"]] | dev
    _check_pyodide_versions(result)
    return result

BUNDLE_VERSION_INFO = _make_bundle_version_info([
    {
        "name": "0.26.0a2",
        "released": True,
        "pyodide_version": "0.26.0a2",
        "pyodide_date": "2024-03-01",
        "packages": PACKAGES_20240829_4,
        "backport": "63",
        "integrity": "sha256-xrG65VJvao9GYH07C73Uq2jA9DW7O1DP16fiZo36Xq0=",
        "flag": "pythonWorkers",
        "enable_flag_name": "python_workers",
        "emscripten_version": "3.1.52",
        "python_version": "3.12.1",
        "baseline_snapshot": "baseline-d13ce2f4a.bin",
        "baseline_snapshot_hash": "d13ce2f4a0ade2e09047b469874dacf4d071ed3558fec4c26f8d0b99d95f77b5",
        "numpy_snapshot": "ew-py-package-snapshot_numpy-v2.bin",
        "numpy_snapshot_hash": "5055deb53f404afacba73642fd10e766b123e661847e8fdf4f1ec92d8ca624dc",
        "fastapi_snapshot": "ew-py-package-snapshot_fastapi-v2.bin",
        "fastapi_snapshot_hash": "d204956a074cd74f7fe72e029e9a82686fcb8a138b509f765e664a03bfdd50fb",
        "vendored_packages_for_tests": [
            {
                # Downloaded from https://pub-25a5b2f2f1b84655b185a505c7a3ad23.r2.dev/beautifulsoup4-vendored-for-ew-testing.zip
                "name": "beautifulsoup4",
                "abi": None,
                "sha256": "5aa09c5f549443969dda260a70e58e3ac8537bd3d29155b307a3d98b36eb70fd",
            },
            {
                # Downloaded from https://pub-25a5b2f2f1b84655b185a505c7a3ad23.r2.dev/fastapi-312-vendored-for-ew-testing.zip
                "name": "fastapi",
                "abi": "3.12",
                "sha256": "5e6e21dbeda7c1eaadb99e6e52aa2ce45325b51e9a417198701e68e0cfd12a4c",
            },
        ],
    },
    {
        "name": "0.28.2",
        "released": True,
        "pyodide_version": "0.28.2",
        "pyodide_date": "2025-01-16",
        "packages": PACKAGES_20250808,
        "backport": "3",
        "integrity": "sha256-SCMwCLKzdE65vBQmdeUPs1enbE8TzOu57LBupZzwJY4=",
        "flag": "pythonWorkers20250116",
        "enable_flag_name": "python_workers_20250116",
        "emscripten_version": "4.0.9",
        "python_version": "3.13.2",
        "baseline_snapshot": "baseline-642052406.bin",
        "baseline_snapshot_hash": "6420524066bd8b40857bedc63d51725fe908051d1707c3515f8699c2f5e1978d",
        "numpy_snapshot": "package_snapshot_numpy-7e7fe731d.bin",
        "numpy_snapshot_hash": "7e7fe731d6e92ebff2535eb3b0960abb8e384a909e642f3f77b4e5c7ec7e86c7",
        "fastapi_snapshot": "package_snapshot_fastapi-2596aa5fd.bin",
        "fastapi_snapshot_hash": "2596aa5fd64c1df9430ffabb96a9a68060086cec8853e1d9f94ad88ab5980ee4",
        "vendored_packages_for_tests": [
            {
                # Downloaded from https://pub-25a5b2f2f1b84655b185a505c7a3ad23.r2.dev/beautifulsoup4-vendored-for-ew-testing.zip
                "name": "beautifulsoup4",
                "abi": None,
                "sha256": "5aa09c5f549443969dda260a70e58e3ac8537bd3d29155b307a3d98b36eb70fd",
            },
            {
                "name": "fastapi",
                "abi": "3.13",
                "sha256": "955091f1bd2eb33255ff2633df990bedc96e2f6294e78f2b416078777394f942",
            },
        ],
    },
    {
        "real_pyodide_version": "0.26.0a2",
        "name": "development",
        "pyodide_version": "dev",
        "pyodide_date": "dev",
        "id": "dev",
        "flag": "pythonWorkersDevPyodide",
        "enable_flag_name": "python_workers_development",
    },
])
