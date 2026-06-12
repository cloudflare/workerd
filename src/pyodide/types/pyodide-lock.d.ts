// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

type InstallDir = 'site' | 'stdlib' | 'dynlib';
// The checked-in lock files are filtered down to just the fields that are still
// consumed: file_name + install_dir for the runtime loader, and sha256 for the
// build-time wheel download.
interface PackageDeclaration {
  file_name: string;
  install_dir: InstallDir;
  sha256: string;
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
