import { parseTarInfo } from 'pyodide-internal:tar';
import { createTarFS } from 'pyodide-internal:tarfs';
import { createMetadataFS } from 'pyodide-internal:metadatafs';
import {
  REQUIREMENTS,
  LOAD_WHEELS_FROM_R2,
  LOCKFILE,
  LOAD_WHEELS_FROM_ARTIFACT_BUNDLER,
  USING_OLDEST_PACKAGES_VERSION,
} from 'pyodide-internal:metadata';
import { simpleRunPython } from 'pyodide-internal:util';
import { default as EmbeddedPackagesTarReader } from 'pyodide-internal:packages_tar_reader';
import { default as MetadataReader } from 'pyodide-internal:runtime-generated/metadata';

const canonicalizeNameRegex = /[-_.]+/g;

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
  constructor() {
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
  mountOverlay(overlayInfo: TarFSInfo, dir: InstallDir): void {
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
  addSmallBundle(
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
  addBigBundle(
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

  getSitePackagesRoot(): TarFSInfo {
    return this.rootInfo;
  }

  getDynlibRoot(): TarFSInfo {
    return this.dynlibTarFs;
  }

  getSoFilesToLoad(): FilePath[] {
    return this.soFiles;
  }

  hasRequirementLoaded(req: string): boolean {
    return this.loadedRequirements.has(req);
  }

  mount(Module: Module, tarFS: EmscriptenFS<TarFSInfo>) {
    const path = getSitePackagesPath(Module);
    Module.FS.mount(tarFS, { info: this.rootInfo }, path);
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
export function buildVirtualizedDir(requirements: Set<string>): VirtualizedDir {
  if (EmbeddedPackagesTarReader.read === undefined) {
    // Package retrieval is enabled, so the embedded tar reader isn't initialized.
    // All packages, including STDLIB_PACKAGES, are loaded in `loadPackages`.
    return new VirtualizedDir();
  }

  const [bigTarInfo, bigTarSoFiles] = parseTarInfo(EmbeddedPackagesTarReader);

  let requirementsInBigBundle = new Set([...STDLIB_PACKAGES]);

  // Currently, we include all packages within the big bundle in Edgeworker.
  // During this transitionary period, we add the option (via autogate)
  // to load packages from GCS (in which case they are accessible through the ArtifactBundler)
  // or to simply use the packages within the big bundle. The latter is not ideal
  // since we're locked to a specific packages version, so we will want to move away
  // from it eventually.
  if (!LOAD_WHEELS_FROM_R2 && !LOAD_WHEELS_FROM_ARTIFACT_BUNDLER) {
    requirements.forEach((r) => requirementsInBigBundle.add(r));
  }

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

export function getSitePackagesPath(Module: Module): string {
  const pymajor = Module._py_version_major();
  const pyminor = Module._py_version_minor();
  return `/session/lib/python${pymajor}.${pyminor}/site-packages`;
}

export const DYNLIB_PATH = '/usr/lib';

/**
 * This mounts a TarFS representing the site-packages directory (which contains the Python packages)
 * and another TarFS representing the dynlib directory (where dynlibs like libcrypto.so live).
 *
 * This has to work before the runtime is initialized because of memory snapshot
 * details, so even though we want these directories to be on sys.path, we
 * handle that separately in adjustSysPath.
 */
export function mountSitePackages(Module: Module, pkgs: VirtualizedDir): void {
  const tarFS = createTarFS(Module);
  const site_packages = getSitePackagesPath(Module);
  Module.FS.mkdirTree(site_packages);
  Module.FS.mkdirTree(DYNLIB_PATH);
  if (!LOAD_WHEELS_FROM_R2 && !LOAD_WHEELS_FROM_ARTIFACT_BUNDLER) {
    // if we are not loading additional wheels, then we're done
    // with site-packages and we can mount it here. Otherwise, we must mount it in
    // loadPackages().
    pkgs.mount(Module, tarFS);
  }
}

/**
 * This mounts the metadataFS (which contains user code).
 */
export function mountWorkerFiles(Module: Module) {
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
  const site_packages = getSitePackagesPath(Module);
  simpleRunPython(
    Module,
    `import sys; sys.path.append("/session/metadata"); sys.path.append("${site_packages}"); del sys`
  );
}

function recursiveDependencies(
  lockfile: PackageLock,
  names: string[]
): PackageDeclaration[] {
  const toLoad = new Map();
  for (const name of names) {
    addPackageToLoad(lockfile, name, toLoad);
  }
  return Array.from(toLoad.values());
}

/**
 * Recursively add a package and its dependencies to toLoad.
 * A helper function for recursiveDependencies.
 * @param name The package to add
 * @param toLoad The set of names of packages to load
 * @private
 */
function addPackageToLoad(
  lockfile: PackageLock,
  name: string,
  toLoad: Map<string, PackageDeclaration>
): void {
  const normalizedName = canonicalizePackageName(name);
  if (toLoad.has(normalizedName)) {
    return;
  }
  const pkgInfo = lockfile.packages[normalizedName];
  if (!pkgInfo) {
    throw new Error(
      `It appears that a package ("${name}") you requested is not available yet in workerd. \n` +
        'If you would like this package to be included, please open an issue at https://github.com/cloudflare/workerd/discussions/new?category=python-packages.'
    );
  }

  toLoad.set(normalizedName, pkgInfo);

  for (let depName of pkgInfo.depends) {
    addPackageToLoad(lockfile, depName, toLoad);
  }
}

export { REQUIREMENTS };
export const TRANSITIVE_REQUIREMENTS = new Set(
  MetadataReader.getTransitiveRequirements()
);
export const VIRTUALIZED_DIR = buildVirtualizedDir(TRANSITIVE_REQUIREMENTS);
