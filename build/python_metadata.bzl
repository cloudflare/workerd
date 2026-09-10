# After updating this file, make sure to run "bazel mod tidy"
load("@bazel_lib//lib:base64.bzl", "base64")
load("@bazel_lib//lib:strings.bzl", "chr")

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
    {
        "version": "314.0.4",
        "sha256": "e775e35fe447beeceaf6f33ab2d2242454a8b6e5401d9769d118eca164d234db",
    },
    {
        "version": "314.0.6",
        "sha256": "1016c31e39ce3764d9a418cbb491a392c802c1b86ccc1367f009f5c59bf8f5fd",
    },
]

# The below is a list of package tags for the old builtin packages support.
#
# Now that built-in package support is gone, the only packages we load are the CPython stdlib
# modules and the shared libraries they depend on. Newer Pyodide versions bundle all of these
# builtin modules directly into the core distribution, so future package bundle versions won't
# need a lock file (or per-package wheel downloads) here at all.
PYTHON_LOCKFILES = [
    {
        "tag": "20240829.4",
    },
    {
        "tag": "20250808",
    },
]

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
            entry["real_pyodide_version"] = entry["pyodide_version"]
        entry["feature_flags"] = [entry["flag"]]
        entry["feature_string_flags"] = [entry["enable_flag_name"]]
        _add_integrity(entry)
        result[name] = entry
        _make_vendored_packages(entry)

    dev = result["development"]

    # Uncomment to test with development = 0.26.0a2
    # dev["real_pyodide_version"] = "0.26.0a2"
    result["development"] = result[dev["real_pyodide_version"]] | dev
    _check_pyodide_versions(result)
    return result

VENDORED_VERSION_INDEPENDENT = [
    {
        # Downloaded from https://pub-25a5b2f2f1b84655b185a505c7a3ad23.r2.dev/beautifulsoup4-vendored-for-ew-testing.zip
        "name": "beautifulsoup4",
        "abi": None,
        "sha256": "5aa09c5f549443969dda260a70e58e3ac8537bd3d29155b307a3d98b36eb70fd",
    },
    {
        "name": "pytest-asyncio",
        "abi": None,
        "sha256": "be25b788392d124cbdfbb9b3d13541da69a7b6b977bc9474dddaddeaab6421b4",
    },
    {
        "name": "python-workers-runtime-sdk",
        "abi": None,
        "sha256": "fc4fb50f73973c257277155b3cb113aa2cf68e9da8ef424ecb049b41bc463183",
    },
]

BUNDLE_VERSION_INFO = _make_bundle_version_info([
    {
        "name": "0.26.0a2",
        "released": True,
        "pyodide_version": "0.26.0a2",
        "pyodide_date": "2024-03-01",
        "packages": "20240829.4",
        "backport": "84",
        "integrity": "sha256-pwZ73EU5MG72in/L9pbb4jx54SJ4Gan17IpZDk+zuo0=",
        "flag": "pythonWorkers",
        "enable_flag_name": "python_workers",
        "emscripten_version": "3.1.52",
        "python_version": "3.12.1",
        "baseline_snapshot": "baseline-61eedf943.bin",
        "baseline_snapshot_hash": "61eedf9432d635bdf091b26efece020b3543429a609fad7af9e8d4de2ec44f47",
        "vendored_packages_for_tests": VENDORED_VERSION_INDEPENDENT + [
            {
                # Downloaded from https://pub-25a5b2f2f1b84655b185a505c7a3ad23.r2.dev/fastapi-312-vendored-for-ew-testing.zip
                "name": "fastapi",
                "abi": "3.12",
                "sha256": "5e6e21dbeda7c1eaadb99e6e52aa2ce45325b51e9a417198701e68e0cfd12a4c",
            },
            {
                "name": "scipy",
                "abi": "3.12",
                "sha256": "787e45be6969a5609093b3df9cc2dba2afec9e10bace977f5045697cc329aa7c",
            },
        ],
    },
    {
        "name": "0.28.2",
        "released": True,
        "pyodide_version": "0.28.2",
        "pyodide_date": "2025-01-16",
        "packages": "20250808",
        "backport": "15",
        "integrity": "sha256-5iDL/nW4k1cCImd156LvkajxPgr9u7m+UePFHcxj9l4=",
        "flag": "pythonWorkers20250116",
        "enable_flag_name": "python_workers_20250116",
        "emscripten_version": "4.0.9",
        "python_version": "3.13.2",
        "baseline_snapshot": "baseline-4569679fb.bin",
        "baseline_snapshot_hash": "4569679fb78a3c5c8dbfa73d57c61c6a5394617632fbac7b5873ba322c85463d",
        "dedicated_fastapi_snapshot": "snapshot_a6b652a95810783f5078b9a5dbd4a07c30718acb4ff724e82c25db7353dd7f2d.bin",
        "dedicated_fastapi_snapshot_hash": "4af6f012a5fb32f31a426e6f109e88ae85b18ee3dd131e1caaaad989cd962bbe",
        "vendored_packages_for_tests": VENDORED_VERSION_INDEPENDENT + [
            {
                "name": "fastapi",
                "abi": "3.13",
                "sha256": "955091f1bd2eb33255ff2633df990bedc96e2f6294e78f2b416078777394f942",
            },
            {
                "name": "numpy",
                "abi": "3.13",
                "sha256": "dc77accd1313a87dadd2ed31bffad3b698dcb9829804e84fc857a9a669a94d3f",
            },
            {
                "name": "shapely",
                "abi": "3.13",
                "sha256": "2e5c462cb32ee8697b3647dfc9d5c88dcdfd0702da34a2d7dc6b07b8090dd321",
            },
        ],
    },
    {
        "name": "314.0.4",
        "released": True,
        "test_by_default": False,
        "pyodide_version": "314.0.4",
        "pyodide_date": "2026-06-10",
        "backport": "15",
        "integrity": "sha256-QF3NLzeV2QWH8uEj7CFogWptnEZEITN004v6bVzo62Q=",
        "flag": "pythonWorkers20260610",
        "enable_flag_name": "python_workers_20260610",
        "emscripten_version": "5.0.3",
        "python_version": "3.14.2",
        "baseline_snapshot": "baseline-2ffc5dfa7.bin",
        "baseline_snapshot_hash": "2ffc5dfa7f3501356b7ecd8cc3a7fe351fbf825346175c1d852d2d9bb5617a48",
        "vendored_packages_for_tests": VENDORED_VERSION_INDEPENDENT + [
            {
                "name": "numpy",
                "abi": "3.14",
                "sha256": "28bea03aa0a18bbc1884ea4cebe8d93a9004455c497417e172c221f0a245b439",
            },
        ],
    },
    {
        "name": "314.0.6",
        "pyodide_version": "314.0.6",
        "pyodide_date": "2026-08-17",
        "backport": "2",
        "integrity": "sha256-x7rlqnQKYsWtgX0QP0BBVbJI6rP0XL1SuMXl99hYhEE=",
        "flag": "pythonWorkers314",
        "enable_flag_name": "python_workers_314",
        "emscripten_version": "5.0.3",
        "python_version": "3.14.2",
        "baseline_snapshot": "baseline-68629a784.bin",
        "baseline_snapshot_hash": "68629a7847e055b24e41a90ea3d64cdb5e9841f4104c2e56b28e5507eab7facd",
        "vendored_packages_for_tests": VENDORED_VERSION_INDEPENDENT + [
            {
                "name": "numpy",
                "abi": "3.14",
                "sha256": "28bea03aa0a18bbc1884ea4cebe8d93a9004455c497417e172c221f0a245b439",
            },
        ],
    },
    {
        "real_pyodide_version": "314.0.6",
        "name": "development",
        "test_by_default": True,
        "pyodide_version": "dev",
        "pyodide_date": "dev",
        "id": "dev",
        "flag": "pythonWorkersDevPyodide",
        "enable_flag_name": "python_workers_development",
    },
])

# Version names from BUNDLE_VERSION_INFO that the test macros expand `python_flags = "all"` into.
# An entry opts out with `"test_by_default": False`, which keeps it selectable by an explicit
# `python_flags` list while removing it from default expansion.
DEFAULT_PYTHON_TEST_FLAGS = [
    name
    for name, entry in BUNDLE_VERSION_INFO.items()
    if entry.get("test_by_default") != False
]
