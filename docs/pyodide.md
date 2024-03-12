# Pyodide Package Indices

workerd is linked against a Pyodide lock file, which is located within an R2 bucket. At build time this lock file is fetched and bundled into the binary. (See WORKSPACE and search for `pyodide-lock.json`)

If you know where the R2 bucket is (See build/pyodide_bucket.bzl) then the `pyodide-lock.json` file is located inside the root of the R2 directory for the Pyodide package bundle release.

This lock file contains some information used by workerd to pull in package requirements, including but not limited to:
- The versions of each package included in the package bundle
- The file names and SHA hashes of each package available for download in the bucket
- What the dependencies are for each package

## Generating pyodide_bucket.bzl
We have scripts and GitHub actions set up for building and uploading Pyodide package bundles onto R2. These are available [here](https://github.com/cloudflare/pyodide-build-scripts). Simply follow the instructions on that repo to build a new version of Pyodide or a new package bundle release.

