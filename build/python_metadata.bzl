load("//:build/python/packages_20240829_4.bzl", "PACKAGES_20240829_4")
load("//:build/python/packages_20241218.bzl", "PACKAGES_20241218")

# The below is a list of pyodide-lock.json files for each package bundle version that we support.
# Each of these gets embedded in the workerd and EW binary.
#
# The key is the `packages` field in pythonSnapshotRelease and the value is the sha256 checksum of
# the lock file.
PYTHON_LOCKFILES = {
    "20240829.4": "c2d9c67ea55a672b95a3beb8d66bfbe7df736edb4bb657383b263151e7e85ef4",
    "20241218": "1421e9351baf24ec44d82f78b9ac26e8e0e6595bfe3f626dedb33147bfcd1998",
}

# This is a dictionary mapping a Python version with its packages version.
#
# NOTE: this needs to be kept in sync with compatibility-date.capnp.
PYTHON_VERSION_TO_PACKAGES = {
    "0.26.0a2": "20240829.4",
    "0.27.1": "20241218",
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
    "20241218": PACKAGES_20241218,
}

# Each new package bundle should contain the same packages as the previous. We verify this
# constraint here.
def verify_no_packages_were_removed():
    package_dates = PYTHON_IMPORTS_TO_TEST.keys()  # Assuming dict order is stable in Skylark.
    for i in range(0, len(package_dates) - 1):
        curr_pkgs = PYTHON_IMPORTS_TO_TEST[package_dates[i]]
        next_pkgs = PYTHON_IMPORTS_TO_TEST[package_dates[i + 1]]
        for pkg in curr_pkgs:
            if pkg not in next_pkgs:
                fail(pkg + " from packages version ", package_dates[i], " not in ", package_dates[i + 1])

verify_no_packages_were_removed()
