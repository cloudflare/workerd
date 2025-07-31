load("@aspect_bazel_lib//lib:base64.bzl", "base64")
load("@aspect_bazel_lib//lib:strings.bzl", "chr")
load("//:build/python/packages_20240829_4.bzl", "PACKAGES_20240829_4")
load("//:build/python/packages_20250616.bzl", "PACKAGES_20250616")

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
    PACKAGES_20250616,
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
    dev = result["development"]

    # Uncomment to test with development = 0.27.7
    # dev["real_pyodide_version"] = "0.27.7"
    result["development"] = result[dev["real_pyodide_version"]] | dev
    return result

BUNDLE_VERSION_INFO = _make_bundle_version_info([
    {
        "name": "0.26.0a2",
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
                "sha256": "5aa09c5f549443969dda260a70e58e3ac8537bd3d29155b307a3d98b36eb70fd",
            },
            {
                # Downloaded from https://pub-25a5b2f2f1b84655b185a505c7a3ad23.r2.dev/fastapi-vendored-for-ew-testing.zip
                "name": "fastapi",
                "sha256": "5e6e21dbeda7c1eaadb99e6e52aa2ce45325b51e9a417198701e68e0cfd12a4c",
            },
        ],
    },
    {
        "name": "0.27.7",
        "pyodide_version": "0.27.7",
        "pyodide_date": "2025-01-16",
        "packages": PACKAGES_20250616,
        "backport": "2",
        "integrity": "sha256-04qtaf3jr6q7mixWrpeASgYzTW1WHb9NEILBGl8M9hk=",
        "flag": "pythonWorkers20250116",
        "enable_flag_name": "python_workers_20250116",
        "emscripten_version": "3.1.58",
        "python_version": "3.12.7",
        "baseline_snapshot": "baseline-59fa311f4.bin",
        "baseline_snapshot_hash": "59fa311f4af0bb28477e2fa17f54dc254ec7fa6f02617b832b141854e44bd621",
        "numpy_snapshot": "package_snapshot_numpy-429b1174f.bin",
        "numpy_snapshot_hash": "429b1174f9c0d73f9c845007c60595c0a80141b440c080c862568f9d2351dcbb",
        "fastapi_snapshot": "package_snapshot_fastapi-23337a32b.bin",
        "fastapi_snapshot_hash": "23337a032bb78f8c2d1abb9439a9c16f56c50130b67aff6bf82b78c896d9a1cc",
        "vendored_packages_for_tests": [
            {
                # Downloaded from https://pub-25a5b2f2f1b84655b185a505c7a3ad23.r2.dev/beautifulsoup4-vendored-for-ew-testing.zip
                "name": "beautifulsoup4",
                "sha256": "5aa09c5f549443969dda260a70e58e3ac8537bd3d29155b307a3d98b36eb70fd",
            },
            {
                # Downloaded from https://pub-25a5b2f2f1b84655b185a505c7a3ad23.r2.dev/fastapi-vendored-for-ew-testing.zip
                "name": "fastapi",
                "sha256": "5e6e21dbeda7c1eaadb99e6e52aa2ce45325b51e9a417198701e68e0cfd12a4c",
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
