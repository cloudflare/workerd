load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive", "http_file")
load("//:build/pyodide_bucket.bzl", "PYODIDE_ALL_WHEELS_ZIP_SHA256", "PYODIDE_GITHUB_RELEASE_URL")
load("//:build/python_metadata.bzl", "PYODIDE_VERSIONS", "PYTHON_LOCKFILES")

def dep_pyodide():
    # Use @workerd prefix on build_file so we can use this from edgeworker too
    for info in PYODIDE_VERSIONS:
        version = info["version"]
        sha256 = info["sha256"]
        http_archive(
            name = "pyodide-%s" % version,
            build_file = "@workerd//:build/BUILD.pyodide",
            sha256 = sha256,
            urls = ["https://github.com/pyodide/pyodide/releases/download/%s/pyodide-core-%s.tar.bz2" % (version, version)],
        )

    for package_date, package_lock_sha in PYTHON_LOCKFILES.items():
        http_file(
            name = "pyodide-lock_" + package_date + ".json",
            sha256 = package_lock_sha,
            url = "https://github.com/cloudflare/pyodide-build-scripts/releases/download/" + package_date + "/pyodide-lock.json",
        )

    http_archive(
        name = "all_pyodide_wheels",
        build_file = "@workerd//:build/BUILD.all_pyodide_wheels",
        sha256 = PYODIDE_ALL_WHEELS_ZIP_SHA256,
        urls = [PYODIDE_GITHUB_RELEASE_URL + "all_wheels.zip"],
    )
