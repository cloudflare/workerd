// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

/**
 * This file mounts the CPython standard library into the Pyodide filesystem: the stdlib zip, and
 * the stdlib packages (and the shared libraries they depend on).
 *
 * The packages are extracted at build time and embedded directly in the Pyodide bundle as
 * individual files (see src/pyodide/pack_python_packages.py and python_packages.capnp), so there is
 * no gzip/tar work and no download at runtime: we simply overlay each embedded file onto
 * site-packages or /usr/lib according to its install_dir.
 *
 * Both are served by read-only filesystems that read straight from the process-wide bundle, so
 * isolates share one copy of the bytes instead of each holding its own.
 */

import { default as EmbeddedPackages } from 'pyodide-internal:packages';
import { default as PythonStdlibZip } from 'pyodide-internal:python_stdlib';
import { VIRTUALIZED_DIR } from 'pyodide-internal:setupPackages';
import { createTarFS } from 'pyodide-internal:tarfs';

// The directory the stdlib zip is mounted at.
// This is symlinked to /lib/pythonXY.zip during the initialization.
// Q. Why do we need to symlink this directory?
// A. 1) A file cannot be mounted directly, it should be a directory.
//    2) We want this file to be read-only, but want to remain /lib directory as writable.
//       So we cannot mount it to /lib directory directly.
const STDLIB_ZIP_DIR = '/lib/stdlib-zip';

/**
 * Mounts the stdlib zip and every embedded stdlib package file. Has to run before the Python
 * runtime starts, since CPython imports from the stdlib zip during startup.
 */
export function loadPackages(Module: Module): void {
  const tarFS = createTarFS(Module);
  mountStdlibZip(Module, tarFS);

  // A single bulk call (each entry carries its reader) rather than a reader accessor per file, to
  // avoid a JS<->C++ round-trip for every stdlib file.
  const files = EmbeddedPackages.getFiles();
  for (const file of files) {
    VIRTUALIZED_DIR.addFile(file.installDir, file.path, file.reader, file.size);
  }

  VIRTUALIZED_DIR.mount(Module, tarFS);
}

/**
 * Mounts the stdlib zip on a read-only filesystem and links /lib/pythonXY.zip to it.
 */
function mountStdlibZip(Module: Module, tarFS: EmscriptenFS<TarFSInfo>): void {
  const [pymajor, pyminor] = Module.API.pyVersionTuple;
  const name = `python${pymajor}${pyminor}.zip`;
  const zipInfo: TarFSInfo = {
    children: undefined,
    mode: 0o444,
    modtime: 0,
    size: PythonStdlibZip.getSize(),
    contentsOffset: 0,
    reader: PythonStdlibZip,
  };
  const rootInfo: TarFSInfo = {
    children: new Map([[name, zipInfo]]),
    mode: 0o555,
    modtime: 0,
    size: 0,
    reader: null,
  };
  Module.FS.mkdirTree(STDLIB_ZIP_DIR);
  Module.FS.mount(tarFS, { info: rootInfo }, STDLIB_ZIP_DIR);

  Module.FS.symlink(`${STDLIB_ZIP_DIR}/${name}`, `/lib/${name}`);
}
