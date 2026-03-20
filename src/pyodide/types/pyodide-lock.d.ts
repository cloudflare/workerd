// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

type InstallDir = 'site' | 'stdlib' | 'dynlib';
interface PackageDeclaration {
  depends: string[];
  file_name: string;
  imports: string[];
  install_dir: InstallDir;
  name: string;
  package_type: string;
  sha256: string;
  shared_library: boolean;
  unvendored_tests: boolean;
  version: string;
}

interface PackageLock {
  packages: {
    [id: string]: PackageDeclaration;
  };
}

declare module 'pyodide-internal:generated/pyodide-lock.json' {
  const lock: PackageLock;
  export default lock;
}
