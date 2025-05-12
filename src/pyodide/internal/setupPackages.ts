import { parseTarInfo } from 'pyodide-internal:tar';
import { createMetadataFS } from 'pyodide-internal:metadatafs';
import { LOCKFILE } from 'pyodide-internal:metadata';
import { simpleRunPython } from 'pyodide-internal:util';
import { default as EmbeddedPackagesTarReader } from 'pyodide-internal:packages_tar_reader';

const canonicalizeNameRegex = /[-_.]+/g;
const DYNLIB_PATH = '/usr/lib';

/**
 * Canonicalize a package name. Port of Python's packaging.utils.canonicalize_name.
 * @param name The package name to canonicalize.
 * @returns The canonicalize package name.
 * @private
 */
function canonicalizePackageName(name: string): string {
  return name.replace(canonicalizeNameRegex, '-').toLowerCase();
}

// The "name" field in the lockfile is not canonicalized
export const STDLIB_PACKAGES: string[] = Object.values(LOCKFILE.packages)
  .filter(({ package_type }) => package_type === 'cpython_module')
  .map(({ name }) => canonicalizePackageName(name));

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
  private rootInfo: TarFSInfo; // site-packages directory
  private dynlibTarFs: TarFSInfo; // /usr/lib directory
  private soFiles: FilePath[];
  private loadedRequirements: Set<string>;
  public constructor() {
    this.rootInfo = createTarFsInfo();
    this.dynlibTarFs = createTarFsInfo();
    this.soFiles = [];
    this.loadedRequirements = new Set();
  }

  /**
   * mountOverlay "overlays" a directory onto the site-packages root directory.
   * All files and subdirectories in the overlay will be accessible at site-packages by the worker.
   * If a file or directory already exists, an error is thrown.
   * @param {TarInfo} overlayInfo The directory that is to be "copied" into site-packages
   */
  public mountOverlay(overlayInfo: TarFSInfo, dir: InstallDir): void {
    const dest = dir == 'dynlib' ? this.dynlibTarFs : this.rootInfo;
    overlayInfo.children!.forEach((val, key) => {
      if (dest.children!.has(key)) {
        throw new Error(
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
   * @param {String} requirement The canonicalized package name this small bundle corresponds to
   * @param {InstallDir} installDir The `install_dir` field from the metadata about the package taken from the lockfile
   */
  public addSmallBundle(
    tarInfo: TarFSInfo,
    soFiles: string[],
    requirement: string,
    installDir: InstallDir
  ): void {
    for (const soFile of soFiles) {
      this.soFiles.push(soFile.split('/'));
    }
    this.mountOverlay(tarInfo, installDir);
    this.loadedRequirements.add(requirement);
  }

  /**
   * A big bundle contains multiple packages, each package contained in a folder whose name is the canonicalized package name.
   * This function overlays the requested packages onto the site-packages directory.
   * @param {TarInfo} tarInfo The root tarInfo for the big bundle (See tar.js)
   * @param {List<String>} soFiles A list of .so files contained in the big bundle
   * @param {List<String>} requirements canonicalized list of packages to pick from the big bundle
   */
  public addBigBundle(
    tarInfo: TarFSInfo,
    soFiles: string[],
    requirements: Set<string>
  ): void {
    // add all the .so files we will need to preload from the big bundle
    for (const soFile of soFiles) {
      // If folder is in list of requirements include .so file in list to preload.
      const [pkg, ...rest] = soFile.split('/');
      if (requirements.has(pkg)) {
        this.soFiles.push(rest);
      }
    }

    for (const req of requirements) {
      const child = tarInfo.children!.get(req);
      if (!child) {
        throw new Error(`Requirement ${req} not found in pyodide packages tar`);
      }
      this.mountOverlay(child, 'site');
      this.loadedRequirements.add(req);
    }
  }

  public getSitePackagesRoot(): TarFSInfo {
    return this.rootInfo;
  }

  public getDynlibRoot(): TarFSInfo {
    return this.dynlibTarFs;
  }

  public getSoFilesToLoad(): FilePath[] {
    return this.soFiles;
  }

  public hasRequirementLoaded(req: string): boolean {
    return this.loadedRequirements.has(req);
  }

  public mount(Module: Module, tarFS: EmscriptenFS<TarFSInfo>): void {
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
 * This stitches together the view of the site packages directory. Each
 * requirement corresponds to a folder in the original tar file. For each
 * requirement in the list we grab the corresponding folder and stitch them
 * together into a combined folder.
 *
 * This also returns the list of soFiles in the resulting site-packages
 * directory so we can preload them.
 *
 * TODO(later): This needs to be removed when external package loading is enabled.
 */
export function buildVirtualizedDir(): VirtualizedDir {
  if (EmbeddedPackagesTarReader.read === undefined) {
    // Package retrieval is enabled, so the embedded tar reader isn't initialized.
    // All packages, including STDLIB_PACKAGES, are loaded in `loadPackages`.
    return new VirtualizedDir();
  }

  const [bigTarInfo, bigTarSoFiles] = parseTarInfo(EmbeddedPackagesTarReader);

  const requirementsInBigBundle = new Set(STDLIB_PACKAGES);
  const res = new VirtualizedDir();
  res.addBigBundle(bigTarInfo, bigTarSoFiles, requirementsInBigBundle);

  return res;
}

/**
 * Patch loadPackage:
 *  - in workerd, disable integrity checks
 *  - otherwise, disable it entirely
 *
 * TODO: stop using loadPackage in workerd.
 */
export function patchLoadPackage(pyodide: Pyodide): void {
  pyodide.loadPackage = disabledLoadPackage;
  return;
}

function disabledLoadPackage(): never {
  throw new Error(
    'pyodide.loadPackage is disabled because packages are encoded in the binary'
  );
}

/**
 * This mounts the metadataFS (which contains user code).
 */
export function mountWorkerFiles(Module: Module): void {
  Module.FS.mkdirTree('/session/metadata');
  const mdFS = createMetadataFS(Module);
  Module.FS.mount(mdFS, {}, '/session/metadata');
  simpleRunPython(
    Module,
    `from importlib import invalidate_caches; invalidate_caches(); del invalidate_caches`
  );
}

/**
 * Add the directories created by mountLib to sys.path.
 * Has to run after the runtime is initialized.
 */
export function adjustSysPath(Module: Module): void {
  const site_packages = Module.FS.sessionSitePackages;
  simpleRunPython(
    Module,
    `import sys; sys.path.append("/session/metadata"); sys.path.append("${site_packages}"); del sys`
  );
}

export const VIRTUALIZED_DIR = buildVirtualizedDir();
