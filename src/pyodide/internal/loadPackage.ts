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

function getPackageMetadata(requirement: string): PackageDeclaration {
  const obj = LOCKFILE['packages'][requirement];
  if (!obj) {
    throw new Error('Requirement ' + requirement + ' not found in lockfile');
  }

  return obj;
}

// Helper function to get the path to a package in the Python package bucket
function getPyodidePackagePath(version: string, filename: string): string {
  return `python-package-bucket/${version}/${filename}`;
}

function loadBundleFromArtifactBundler(requirement: string): Promise<Reader> {
  const filename = getPackageMetadata(requirement).file_name;
  const fullPath = getPyodidePackagePath(PACKAGES_VERSION, filename);
  const reader = ArtifactBundler.getPackage(fullPath);
  if (!reader) {
    throw new Error(
      'Failed to get package ' + fullPath + ' from ArtifactBundler'
    );
  }
  return Promise.resolve(reader);
}

async function loadBundle(
  Module: Module,
  requirements: Set<string>
): Promise<void> {
  const loadPromises: Promise<[string, Reader]>[] = [];
  const loading = [];
  for (const req of requirements) {
    if (req === 'test') {
      continue; // Skip the test package, it is only useful for internal Python regression testing.
    }
    if (VIRTUALIZED_DIR.hasRequirementLoaded(req)) {
      continue;
    }
    loadPromises.push(loadBundleFromArtifactBundler(req).then((r) => [req, r]));
    loading.push(req);
  }

  console.log('Loading ' + loading.join(', '));

  const buffers = await Promise.all(loadPromises);
  for (const [requirement, reader] of buffers) {
    const [tarInfo, soFiles] = parseTarInfo(reader);
    const pkg = getPackageMetadata(requirement);
    VIRTUALIZED_DIR.addSmallBundle(
      tarInfo,
      soFiles,
      requirement,
      pkg.install_dir
    );
  }

  console.log('Loaded ' + loading.join(', '));

  const tarFS = createTarFS(Module);
  VIRTUALIZED_DIR.mount(Module, tarFS);
}

/**
 * Downloads the requirements specified and loads them into Pyodide. Note that this does not
 * do any dependency resolution, it just installs the requirements that are specified. See
 * `getTransitiveRequirements` for the code that deals with this.
 */
export async function loadPackages(
  Module: Module,
  requirements: Set<string>
): Promise<void> {
  let pkgsToLoad = requirements;
  // TODO: Package snapshot created with '20240829.4' needs the stdlib packages to be added here.
  // We should remove this check once the next Python and packages versions are rolled
  // out.
  if (USING_OLDEST_PACKAGES_VERSION) {
    pkgsToLoad = pkgsToLoad.union(new Set(STDLIB_PACKAGES));
  }

  await loadBundle(Module, pkgsToLoad);
}
