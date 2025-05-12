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
  WORKERD_INDEX_URL,
  LOCKFILE,
  PACKAGES_VERSION,
  USING_OLDEST_PACKAGES_VERSION,
  IS_WORKERD,
} from 'pyodide-internal:metadata';
import {
  VIRTUALIZED_DIR,
  STDLIB_PACKAGES,
} from 'pyodide-internal:setupPackages';
import { parseTarInfo } from 'pyodide-internal:tar';
import { default as DiskCache } from 'pyodide-internal:disk_cache';
import { createTarFS } from 'pyodide-internal:tarfs';
import { default as ArtifactBundler } from 'pyodide-internal:artifacts';

async function decompressArrayBuffer(
  arrBuf: ArrayBuffer
): Promise<ArrayBuffer> {
  const resp = new Response(arrBuf);
  return await new Response(
    resp.body!.pipeThrough(new DecompressionStream('gzip'))
  ).arrayBuffer();
}

function getPackageMetadata(requirement: string): PackageDeclaration {
  const obj = LOCKFILE['packages'][requirement];
  if (!obj) {
    throw new Error('Requirement ' + requirement + ' not found in lockfile');
  }

  return obj;
}

// loadBundleFromR2 loads the package from the internet (through fetch) and uses the DiskCache as
// a backing store. This is only used in local dev.
async function loadBundleFromR2(requirement: string): Promise<Reader> {
  // first check if the disk cache has what we want
  const filename = getPackageMetadata(requirement).file_name;
  let original = DiskCache.get(filename);
  if (!original) {
    // we didn't find it in the disk cache, continue with original fetch
    const url = new URL(WORKERD_INDEX_URL + filename);
    const response = await fetch(url);
    if (response.status != 200) {
      throw new Error(
        `Could not fetch package at url ${url} received status ${response.status}`
      );
    }

    original = await response.arrayBuffer();
    DiskCache.put(filename, original);
  }

  if (filename.endsWith('.tar.gz')) {
    const decompressed = await decompressArrayBuffer(original);
    return new ArrayBufferReader(decompressed);
  } else if (filename.endsWith('.tar')) {
    return new ArrayBufferReader(original);
  } else {
    throw new Error('Unsupported package file type: ' + filename);
  }
}

async function loadBundleFromR2WithRetry(
  requirement: string,
  currRetry: number
): Promise<Reader> {
  try {
    return await loadBundleFromR2(requirement);
  } catch (exc) {
    if (currRetry < 3) {
      console.warn(`Error loading '${requirement}' from R2: ${exc}`);
      console.log('Retrying fetch in 5 seconds...');
      await new Promise((r) => setTimeout(r, 5000));
      return loadBundleFromR2WithRetry(requirement, currRetry + 1);
    }

    throw new Error(`Could not load '${requirement}' from R2: ${exc}`);
  }
}

function loadBundleFromArtifactBundler(requirement: string): Promise<Reader> {
  const filename = getPackageMetadata(requirement).file_name;
  const fullPath = 'python-package-bucket/' + PACKAGES_VERSION + '/' + filename;
  const reader = ArtifactBundler.getPackage(fullPath);
  if (!reader) {
    throw new Error(
      'Failed to get package ' + fullPath + ' from ArtifactBundler'
    );
  }
  return Promise.resolve(reader);
}

/**
 * ArrayBufferReader wraps around an arrayBuffer in a way that tar.js is able to read from
 */
class ArrayBufferReader {
  public constructor(private arrayBuffer: ArrayBuffer) {}

  public read(offset: number, buf: Uint8Array): number {
    const size = this.arrayBuffer.byteLength;
    if (offset >= size || offset < 0) {
      return 0;
    }
    let toCopy = buf.length;
    if (size - offset < toCopy) {
      toCopy = size - offset;
    }
    buf.set(new Uint8Array(this.arrayBuffer, offset, toCopy));
    return toCopy;
  }
}

async function loadPackagesImpl(
  Module: Module,
  requirements: Set<string>,
  loadBundle: (req: string) => Promise<Reader>
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
    loadPromises.push(loadBundle(req).then((r) => [req, r]));
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
  if (IS_WORKERD) {
    await loadPackagesImpl(Module, pkgsToLoad, (req) =>
      loadBundleFromR2WithRetry(req, 0)
    );
  } else {
    await loadPackagesImpl(Module, pkgsToLoad, loadBundleFromArtifactBundler);
  }
}
