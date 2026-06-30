// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { createMetadataFS } from 'pyodide-internal:metadatafs';
import {
  invalidateCaches,
  PythonWorkersInternalError,
  simpleRunPython,
} from 'pyodide-internal:util';

const DYNLIB_PATH = '/usr/lib';

// Each item in the list is an element of the file path, for example
// `folder/file.txt` -> `["folder", "file.txt"]
export type FilePath = string[];

function createTarFsInfo(): TarFSInfo {
  return {
    children: new Map(),
    mode: 0o777,
    type: '5',
    modtime: 0,
    size: 0,
    path: '',
    name: '',
    parts: [],
    reader: null,
  };
}

/**
 * VirtualizedDir keeps track of the virtualized view of the site-packages
 * directory generated for each worker as well as a virtualized view of the dynamic libraries stored
 * in /usr/lib.
 */
class VirtualizedDir {
  // TODO(soon): Can we use the # syntax here?
  // eslint-disable-next-line no-restricted-syntax
  private rootInfo: TarFSInfo; // site-packages directory
  // TODO(soon): Can we use the # syntax here?
  // eslint-disable-next-line no-restricted-syntax
  private dynlibTarFs: TarFSInfo; // /usr/lib directory
  // TODO(soon): Can we use the # syntax here?
  // eslint-disable-next-line no-restricted-syntax
  private soFiles: FilePath[];
  constructor() {
    this.rootInfo = createTarFsInfo();
    this.dynlibTarFs = createTarFsInfo();
    this.soFiles = [];
  }

  /**
   * Adds a single extracted package file to the virtualized filesystem, creating intermediate
   * directories as needed. Files are routed to /usr/lib (dynlib) or site-packages (everything else)
   * based on their `installDir`. The package stdlib is embedded in the bundle as individual files
   * (see loadPackage.ts), so this is called once per file.
   *
   * @param installDir The package's `install_dir` (from the lock file).
   * @param path The file's path within `installDir`, e.g. "ssl/__init__.py".
   * @param reader Reads the file's contents.
   * @param size The file's size in bytes.
   */
  addFile(
    installDir: InstallDir,
    path: string,
    reader: Reader,
    size: number
  ): void {
    const dest = installDir == 'dynlib' ? this.dynlibTarFs : this.rootInfo;
    const parts = path.split('/');
    let dir = dest;
    for (let i = 0; i < parts.length - 1; i++) {
      const part = parts[i]!;
      let child = dir.children!.get(part);
      if (!child) {
        child = {
          children: new Map(),
          mode: 0o777,
          type: '5',
          modtime: 0,
          size: 0,
          path: parts.slice(0, i + 1).join('/'),
          name: part,
          parts: [],
          reader: null,
        };
        dir.children!.set(part, child);
      }
      if (!child.children) {
        throw new PythonWorkersInternalError(
          `File/folder ${path} conflicts with a file written by another package`
        );
      }
      dir = child;
    }

    const name = parts.at(-1)!;
    if (dir.children!.has(name)) {
      throw new PythonWorkersInternalError(
        `File ${path} being written by multiple packages`
      );
    }
    dir.children!.set(name, {
      children: undefined,
      mode: 0o755,
      type: '0',
      modtime: 0,
      size,
      path,
      name,
      parts: [],
      contentsOffset: 0,
      reader,
    });

    if (path.endsWith('.so')) {
      this.soFiles.push(parts);
    }
  }

  getSitePackagesRoot(): TarFSInfo {
    return this.rootInfo;
  }

  getDynlibRoot(): TarFSInfo {
    return this.dynlibTarFs;
  }

  /** Only used for Pyodide 0.26.0a2 */
  getSoFilesToLoad(): FilePath[] {
    return this.soFiles;
  }

  mount(Module: Module, tarFS: EmscriptenFS<TarFSInfo>): void {
    Module.FS.mkdirTree(Module.FS.sessionSitePackages);
    Module.FS.mount(
      tarFS,
      { info: this.rootInfo },
      Module.FS.sessionSitePackages
    );
    Module.FS.mkdirTree(DYNLIB_PATH);
    Module.FS.mount(tarFS, { info: this.dynlibTarFs }, DYNLIB_PATH);
  }
}

/**
 * This mounts the metadataFS (which contains user code).
 */
export function mountWorkerFiles(Module: Module): void {
  Module.FS.mkdirTree('/session/metadata');
  const mdFS = createMetadataFS(Module);
  Module.FS.mount(mdFS, {}, '/session/metadata');
  invalidateCaches(Module);
}

/**
 * Add the directories created by mountLib to sys.path.
 * Has to run after the runtime is initialized but before memory snapshot is collected.
 */
export function adjustSysPath(Module: Module): void {
  const site_packages = Module.FS.sessionSitePackages;
  simpleRunPython(
    Module,
    `import sys; sys.path.append("/session/metadata"); sys.path.append("${site_packages}"); del sys`
  );
}

export const VIRTUALIZED_DIR = new VirtualizedDir();
