# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

@0xc3f6a2b1e4d50789;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("workerd::api::pyodide");

# A single file extracted from a Python stdlib package wheel at build time. The CPython stdlib
# modules and the shared libraries they depend on are extracted and embedded directly in the
# Pyodide bundle (see src/pyodide/pack_python_packages.py and helpers.bzl) so the runtime no longer
# downloads or unpacks wheels at request time.
struct PythonPackageFile {
  # The mount root this file belongs to, taken from the package's `install_dir` in the lock file
  # ("site" / "stdlib" -> site-packages, "dynlib" -> /usr/lib).
  installDir @0 :Text;
  # The file's path within `installDir`, e.g. "ssl/__init__.py".
  path @1 :Text;
  # The (already-decompressed) file contents.
  contents @2 :Data;
}

struct PythonPackages {
  files @0 :List(PythonPackageFile);
}
