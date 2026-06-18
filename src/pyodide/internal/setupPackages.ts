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
   * mountOverlay "overlays" a directory onto the site-packages root directory.
   * All files and subdirectories in the overlay will be accessible at site-packages by the worker.
   * If a file or directory already exists, an error is thrown.
   * @param {TarInfo} overlayInfo The directory that is to be "copied" into site-packages
   */
  mountOverlay(overlayInfo: TarFSInfo, dir: InstallDir): void {
    const dest = dir == 'dynlib' ? this.dynlibTarFs : this.rootInfo;
    overlayInfo.children!.forEach((val, key) => {
      if (dest.children!.has(key)) {
        throw new PythonWorkersInternalError(
          `File/folder ${key} being written by multiple packages`
        );
      }
      dest.children!.set(key, val);
    });
  }

  /**
   * A small bundle contains just a single package, it can be thought of as a wheel.
   *
   * The entire bundle will be overlaid onto site-packages or /usr/lib depending on its install_dir.
   *
   * @param {TarInfo} tarInfo The root tarInfo for the small bundle (See tar.js)
   * @param {List<String>} soFiles A list of .so files contained in the small bundle
   * @param {InstallDir} installDir The `install_dir` field from the metadata about the package taken from the lockfile
   */
  addSmallBundle(
    tarInfo: TarFSInfo,
    soFiles: string[],
    installDir: InstallDir
  ): void {
    for (const soFile of soFiles) {
      this.soFiles.push(soFile.split('/'));
    }
    this.mountOverlay(tarInfo, installDir);
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
