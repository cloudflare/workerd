// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

/**
 * This file mounts the CPython stdlib packages (and the shared libraries they depend on) into the
 * Pyodide filesystem.
 *
 * The packages are extracted at build time and embedded directly in the Pyodide bundle as
 * individual files (see src/pyodide/pack_python_packages.py and python_packages.capnp), so there is
 * no gzip/tar work and no download at runtime: we simply overlay each embedded file onto
 * site-packages or /usr/lib according to its install_dir.
 */

import { default as EmbeddedPackages } from 'pyodide-internal:packages';
import { VIRTUALIZED_DIR } from 'pyodide-internal:setupPackages';
import { createTarFS } from 'pyodide-internal:tarfs';

/**
 * Mounts every embedded stdlib package file into the virtualized filesystem.
 */
export function loadPackages(Module: Module): void {
  // A single bulk call (each entry carries its reader) rather than a reader accessor per file, to
  // avoid a JS<->C++ round-trip for every stdlib file.
  const files = EmbeddedPackages.getFiles();
  for (const file of files) {
    VIRTUALIZED_DIR.addFile(file.installDir, file.path, file.reader, file.size);
  }

  const tarFS = createTarFS(Module);
  VIRTUALIZED_DIR.mount(Module, tarFS);
}
