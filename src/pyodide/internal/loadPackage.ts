/**
 * This file contains code that roughly replaces pyodide.loadPackage, with workerd-specific
 * optimizations:
 * - Wheels are decompressed with a DecompressionStream instead of in Python
 * - Wheels are overlaid onto the site-packages dir instead of actually being copied
 * - Wheels are fetched from a disk cache if available.
 *
 * Note that loadPackages is only used in local dev for now, internally we use the full big bundle
 * that contains all the packages ready to go.
 */

import {
  LOCKFILE,
  PACKAGES_VERSION,
  USING_OLDEST_PACKAGES_VERSION,
} from 'pyodide-internal:metadata';
import {
  VIRTUALIZED_DIR,
  STDLIB_PACKAGES,
} from 'pyodide-internal:setupPackages';
import { parseTarInfo } from 'pyodide-internal:tar';
import { createTarFS } from 'pyodide-internal:tarfs';
import { default as ArtifactBundler } from 'pyodide-internal:artifacts';
import {
  PythonUserError,
  PythonWorkersInternalError,
} from 'pyodide-internal:util';

function getPackageMetadata(requirement: string): PackageDeclaration {
  const obj = LOCKFILE['packages'][requirement];
  if (!obj) {
    throw new PythonUserError(
      'Requirement ' + requirement + ' not found in lockfile'
    );
  }

  return obj;
}

function loadBundleFromArtifactBundler(requirement: string): Reader {
  const filename = getPackageMetadata(requirement).file_name;
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
 * Downloads the requirements specified and loads them into Pyodide. Note that this does not
 * do any dependency resolution, it just installs the requirements that are specified. See
 * `getTransitiveRequirements` for the code that deals with this.
 */
export function loadPackages(Module: Module, requirements: Set<string>): void {
  let pkgsToLoad = requirements;
  // TODO: Package snapshot created with '20240829.4' needs the stdlib packages to be added here.
  // We should remove this check once the next Python and packages versions are rolled
  // out.
  if (USING_OLDEST_PACKAGES_VERSION) {
    pkgsToLoad = pkgsToLoad.union(new Set(STDLIB_PACKAGES));
  }

  for (const req of pkgsToLoad) {
    if (req === 'test') {
      continue; // Skip the test package, it is only useful for internal Python regression testing.
    }
    if (VIRTUALIZED_DIR.hasRequirementLoaded(req)) {
      continue;
    }

    const reader = loadBundleFromArtifactBundler(req);
    const [tarInfo, soFiles] = parseTarInfo(reader);
    const pkg = getPackageMetadata(req);
    VIRTUALIZED_DIR.addSmallBundle(tarInfo, soFiles, req, pkg.install_dir);
  }

  const tarFS = createTarFS(Module);
  VIRTUALIZED_DIR.mount(Module, tarFS);
}
