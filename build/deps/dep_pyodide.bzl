load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive", "http_file")
load("//:build/pyodide_bucket.bzl", "PYODIDE_ALL_WHEELS_ZIP_SHA256", "PYODIDE_GITHUB_RELEASE_URL")
load("//:build/python_metadata.bzl", "PYTHON_LOCKFILES")

def dep_pyodide():
    http_archive(
        name = "pyodide",
        build_file = "//:build/BUILD.pyodide",
        sha256 = "fbda450a64093a8d246c872bb901ee172a57fe594c9f35bba61f36807c73300d",
        urls = ["https://github.com/pyodide/pyodide/releases/download/0.26.0a2/pyodide-core-0.26.0a2.tar.bz2"],
    )

    for package_date, package_lock_sha in PYTHON_LOCKFILES.items():
        http_file(
            name = "pyodide-lock_" + package_date + ".json",
            sha256 = package_lock_sha,
            url = "https://github.com/cloudflare/pyodide-build-scripts/releases/download/" + package_date + "/pyodide-lock.json",
        )

    http_archive(
        name = "all_pyodide_wheels",
        build_file = "//:build/BUILD.all_pyodide_wheels",
        sha256 = PYODIDE_ALL_WHEELS_ZIP_SHA256,
        urls = [PYODIDE_GITHUB_RELEASE_URL + "all_wheels.zip"],
    )
