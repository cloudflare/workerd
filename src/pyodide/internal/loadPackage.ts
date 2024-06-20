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

import { default as LOCKFILE } from "pyodide-internal:generated/pyodide-lock.json";
import { WORKERD_INDEX_URL } from "pyodide-internal:metadata";
import {
  SITE_PACKAGES,
  LOAD_WHEELS_FROM_R2,
  getSitePackagesPath,
} from "pyodide-internal:setupPackages";
import { parseTarInfo } from "pyodide-internal:tar";
import { default as DiskCache } from "pyodide-internal:disk_cache";
import { createTarFS } from "pyodide-internal:tarfs";

async function decompressArrayBuffer(
  arrBuf: ArrayBuffer,
): Promise<ArrayBuffer> {
  const resp = new Response(arrBuf);
  if (resp && resp.body) {
    return await new Response(
      resp.body.pipeThrough(new DecompressionStream("gzip")),
    ).arrayBuffer();
  } else {
    throw new Error("Failed to decompress array buffer");
  }
}

async function loadBundle(requirement: string): Promise<[string, ArrayBuffer]> {
  // first check if the disk cache has what we want
  const filename = LOCKFILE["packages"][requirement]["file_name"];
  const cached = DiskCache.get(filename);
  if (cached) {
    const decompressed = await decompressArrayBuffer(cached);
    return [requirement, decompressed];
  }

  // we didn't find it in the disk cache, continue with original fetch
  const url = new URL(WORKERD_INDEX_URL + filename);
  const response = await fetch(url);

  const compressed = await response.arrayBuffer();
  const decompressed = await decompressArrayBuffer(compressed);

  DiskCache.put(filename, compressed);
  return [requirement, decompressed];
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

export async function loadPackages(Module: Module, requirements: string[]) {
  if (!LOAD_WHEELS_FROM_R2) return;

  let loadPromises = [];
  let loading = [];
  for (const req of requirements) {
    if (SITE_PACKAGES.loadedRequirements.has(req)) continue;
    loadPromises.push(loadBundle(req));
    loading.push(req);
  }

  console.log("Loading " + loading.join(", "));

  const buffers = await Promise.all(loadPromises);
  for (const [requirement, buffer] of buffers) {
    const reader = new ArrayBufferReader(buffer);
    const [tarInfo, soFiles] = parseTarInfo(reader);
    SITE_PACKAGES.addSmallBundle(tarInfo, soFiles, requirement);
  }

  console.log("Loaded " + loading.join(", "));

  const tarFS = createTarFS(Module);
  const path = getSitePackagesPath(Module);
  const info = SITE_PACKAGES.rootInfo;
  Module.FS.mount(tarFS, { info }, path);
}
