// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

/**
 * This file contains code that roughly replaces pyodide.loadPackage, with workerd-specific
 * optimizations:
 * - Wheels are decompressed with a DecompressionStream instead of in Python
 * - Wheels are overlaid onto the site-packages dir instead of actually being copied
 * - Wheels are fetched from a disk cache if available.
 *
 * Every package in the (pre-filtered) lock file is loaded; see `loadPackages` below.
 */

import { LOCKFILE, PACKAGES_VERSION } from 'pyodide-internal:metadata';
import { VIRTUALIZED_DIR } from 'pyodide-internal:setupPackages';
import { parseTarInfo } from 'pyodide-internal:tar';
import { createTarFS } from 'pyodide-internal:tarfs';
import { default as ArtifactBundler } from 'pyodide-internal:artifacts';
import { PythonWorkersInternalError } from 'pyodide-internal:util';

function loadBundleFromArtifactBundler(meta: PackageDeclaration): Reader {
  const filename = meta.file_name;
  const fullPath = `python-package-bucket/${PACKAGES_VERSION}/${filename}`;
  const reader = ArtifactBundler.getPackage(fullPath);
  if (!reader) {
    throw new PythonWorkersInternalError(
      'Failed to get package ' + fullPath + ' from ArtifactBundler'
    );
  }
  return reader;
}

/**
 * Loads every package in the lock file into Pyodide. Built-in package requirements are no longer
 * supported, and the lock file is pre-filtered to contain exactly the set of packages we want to
 * load (the CPython stdlib modules and the shared libraries they depend on), so we simply load all
 * of them.
 */
export function loadPackages(Module: Module): void {
  for (const meta of Object.values(LOCKFILE.packages)) {
    const reader = loadBundleFromArtifactBundler(meta);
    const [tarInfo, soFiles] = parseTarInfo(reader);
    VIRTUALIZED_DIR.addSmallBundle(tarInfo, soFiles, meta.install_dir);
  }

  const tarFS = createTarFS(Module);
  VIRTUALIZED_DIR.mount(Module, tarFS);
}
