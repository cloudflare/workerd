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
  LOAD_WHEELS_FROM_R2,
  LOAD_WHEELS_FROM_ARTIFACT_BUNDLER,
  PACKAGES_VERSION,
} from 'pyodide-internal:metadata';
import {
  SITE_PACKAGES,
  STDLIB_PACKAGES,
  getSitePackagesPath,
} from 'pyodide-internal:setupPackages';
import { parseTarInfo } from 'pyodide-internal:tar';
import { default as DiskCache } from 'pyodide-internal:disk_cache';
import { createTarFS } from 'pyodide-internal:tarfs';
import { default as ArtifactBundler } from 'pyodide-internal:artifacts';

async function decompressArrayBuffer(
  arrBuf: ArrayBuffer
): Promise<ArrayBuffer> {
  const resp = new Response(arrBuf);
  if (resp && resp.body) {
    return await new Response(
      resp.body.pipeThrough(new DecompressionStream('gzip'))
    ).arrayBuffer();
  } else {
    throw new Error('Failed to decompress array buffer');
  }
}

function getFilenameOfPackage(requirement: string): string {
  const obj = LOCKFILE['packages'][requirement];
  if (!obj) {
    throw new Error('Requirement ' + requirement + ' not found in lockfile');
  }

  return obj.file_name;
}

// loadBundleFromR2 loads the package from the internet (through fetch) and uses the DiskCache as
// a backing store. This is only used in local dev.
async function loadBundleFromR2(requirement: string): Promise<Reader> {
  // first check if the disk cache has what we want
  const filename = getFilenameOfPackage(requirement);
  let original = DiskCache.get(filename);
  if (!original) {
    // we didn't find it in the disk cache, continue with original fetch
    const url = new URL(WORKERD_INDEX_URL + filename);
    const response = await fetch(url);
    if (response.status != 200) {
      throw new Error(
        'Could not fetch package at url ' +
          url +
          ' received status ' +
          response.status
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

async function loadBundleFromArtifactBundler(
  requirement: string
): Promise<Reader> {
  const filename = getFilenameOfPackage(requirement);
  const fullPath = 'python-package-bucket/' + PACKAGES_VERSION + '/' + filename;
  const reader = ArtifactBundler.getPackage(fullPath);
  if (!reader) {
    throw new Error(
      'Failed to get package ' + fullPath + ' from ArtifactBundler'
    );
  }
  return reader;
}

/**
 * ArrayBufferReader wraps around an arrayBuffer in a way that tar.js is able to read from
 */
class ArrayBufferReader {
  constructor(private arrayBuffer: ArrayBuffer) {}

  read(offset: number, buf: Uint8Array): number {
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
) {
  let loadPromises: Promise<[string, Reader]>[] = [];
  let loading = [];
  for (const req of requirements) {
    if (req === 'test') {
      continue; // Skip the test package, it is only useful for internal Python regression testing.
    }
    if (SITE_PACKAGES.loadedRequirements.has(req)) {
      continue;
    }
    loadPromises.push(loadBundle(req).then((r) => [req, r]));
    loading.push(req);
  }

  console.log('Loading ' + loading.join(', '));

  const buffers = await Promise.all(loadPromises);
  for (const [requirement, reader] of buffers) {
    const [tarInfo, soFiles] = parseTarInfo(reader);
    SITE_PACKAGES.addSmallBundle(tarInfo, soFiles, requirement);
  }

  console.log('Loaded ' + loading.join(', '));

  const tarFS = createTarFS(Module);
  const path = getSitePackagesPath(Module);
  const info = SITE_PACKAGES.rootInfo;
  Module.FS.mount(tarFS, { info }, path);
}

export async function loadPackages(Module: Module, requirements: Set<string>) {
  const pkgsToLoad = requirements.union(new Set(STDLIB_PACKAGES));
  if (LOAD_WHEELS_FROM_R2) {
    await loadPackagesImpl(Module, pkgsToLoad, loadBundleFromR2);
  } else if (LOAD_WHEELS_FROM_ARTIFACT_BUNDLER) {
    await loadPackagesImpl(Module, pkgsToLoad, loadBundleFromArtifactBundler);
  }
}
