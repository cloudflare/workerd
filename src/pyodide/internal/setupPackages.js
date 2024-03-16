import { parseTarInfo } from "pyodide-internal:tar";
import { createTarFS } from "pyodide-internal:tarfs";
import { createMetadataFS } from "pyodide-internal:metadatafs";
import { default as LOCKFILE } from "pyodide-internal:generated/pyodide-lock.json";
import { REQUIREMENTS, WORKERD_INDEX_URL } from "pyodide-internal:metadata";
import { patchFetch } from "pyodide-internal:builtin_wrappers";

const canonicalizeNameRegex = /[-_.]+/g;

/**
 * Canonicalize a package name. Port of Python's packaging.utils.canonicalize_name.
 * @param name The package name to canonicalize.
 * @returns The canonicalize package name.
 * @private
 */
function canonicalizePackageName(name) {
  return name.replace(canonicalizeNameRegex, "-").toLowerCase();
}

// The "name" field in the lockfile is not canonicalized
const STDLIB_PACKAGES = Object.values(LOCKFILE.packages)
  .filter(({ install_dir }) => install_dir === "stdlib")
  .map(({ name }) => canonicalizePackageName(name));


/**
 * This stitches together the view of the site packages directory. Each
 * requirement corresponds to a folder in the original tar file. For each
 * requirement in the list we grab the corresponding folder and stitch them
 * together into a combined folder.
 *
 * This also returns the list of soFiles in the resulting site-packages
 * directory so we can preload them.
 */
export function buildSitePackages(requirements) {
  const [origTarInfo, origSoFiles] = parseTarInfo();
  // We'd like to set USE_LOAD_PACKAGE = IS_WORKERD but we also build a funny
  // workerd with the downstream package set. We can distinguish between them by
  // looking at the contents. This uses the fact that the downstream set is
  // larger, but there are a lot of differences...
  const USE_LOAD_PACKAGE = origTarInfo.children.size < 10;
  if (USE_LOAD_PACKAGE) {
    requirements = new Set([...STDLIB_PACKAGES]);
  } else {
    requirements = new Set([...STDLIB_PACKAGES, ...requirements]);
  }
  const soFiles = [];
  for (const soFile of origSoFiles) {
    // If folder is in list of requirements include .so file in list to preload.
    const [pkg, ...rest] = soFile.split("/");
    if (requirements.has(pkg)) {
      soFiles.push(rest);
    }
  }
  const newTarInfo = {
    children: new Map(),
    mode: 0o777,
    type: 5,
    modtime: 0,
    size: 0,
    path: "",
    name: "",
    parts: [],
  };

  for (const req of requirements) {
    const child = origTarInfo.children.get(req);
    if (!child) {
      throw new Error(`Requirement ${req} not found in pyodide packages tar`);
    }
    child.children.forEach((val, key) => {
      if (newTarInfo.children.has(key)) {
        throw new Error(
          `File/folder ${key} being written by multiple packages`,
        );
      }
      newTarInfo.children.set(key, val);
    });
  }

  return [newTarInfo, soFiles, USE_LOAD_PACKAGE];
}

/**
 * Patch loadPackage:
 *  - in workerd, disable integrity checks
 *  - otherwise, disable it entirely
 *
 * TODO: stop using loadPackage in workerd.
 */
export function patchLoadPackage(pyodide) {
  if (!USE_LOAD_PACKAGE) {
    pyodide.loadPackage = disabledLoadPackage;
    return;
  }
  patchFetch(new URL(WORKERD_INDEX_URL).origin);
  const origLoadPackage = pyodide.loadPackage;
  function loadPackage(packages, options) {
    return origLoadPackage(packages, {
      checkIntegrity: false,
      ...options,
    });
  }
  pyodide.loadPackage = loadPackage;
}

function disabledLoadPackage() {
  throw new Error("We only use loadPackage in workerd");
}

/**
 * Get the set of transitive requirements from the REQUIREMENTS metadata.
 */
function getTransitiveRequirements() {
  const requirements = REQUIREMENTS.map(canonicalizePackageName);
  // resolve transitive dependencies of requirements and if IN_WORKERD install them from the cdn.
  const packageDatas = recursiveDependencies(LOCKFILE, requirements);
  return new Set(packageDatas.map(({ name }) => canonicalizePackageName(name)));
}

export function getSitePackagesPath(Module) {
  const pymajor = Module._py_version_major();
  const pyminor = Module._py_version_minor();
  return `/session/lib/python${pymajor}.${pyminor}/site-packages`;
}

/**
 * This mounts the tarFS (which contains the packages) and metadataFS (which
 * contains user code).
 *
 * This has to work before the runtime is initialized because of memory snapshot
 * details, so even though we want these directories to be on sys.path, we
 * handle that separately in adjustSysPath.
 */
export function mountLib(Module, info) {
  const tarFS = createTarFS(Module);
  const mdFS = createMetadataFS(Module);
  const site_packages = getSitePackagesPath(Module);
  Module.FS.mkdirTree(site_packages);
  Module.FS.mkdirTree("/session/metadata");
  Module.FS.mount(tarFS, { info }, site_packages);
  Module.FS.mount(mdFS, {}, "/session/metadata");
}

/**
 * Add the directories created by mountLib to sys.path.
 * Has to run after the runtime is initialized.
 */
export function adjustSysPath(Module, simpleRunPython) {
  const site_packages = getSitePackagesPath(Module);
  simpleRunPython(
    Module,
    `import sys; sys.path.append("/session/metadata"); sys.path.append("${site_packages}"); del sys`,
  );
}

function recursiveDependencies(lockfile, names) {
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
function addPackageToLoad(lockfile, name, toLoad) {
  const normalizedName = canonicalizePackageName(name);
  if (toLoad.has(normalizedName)) {
    return;
  }
  const pkgInfo = lockfile.packages[normalizedName];
  if (!pkgInfo) {
    throw new Error(`No known package with name '${name}'`);
  }

  toLoad.set(normalizedName, pkgInfo);

  for (let depName of pkgInfo.depends) {
    addPackageToLoad(lockfile, depName, toLoad);
  }
}

export { REQUIREMENTS };
export const TRANSITIVE_REQUIREMENTS = getTransitiveRequirements();
export const [SITE_PACKAGES_INFO, SITE_PACKAGES_SO_FILES, USE_LOAD_PACKAGE] = buildSitePackages(
  TRANSITIVE_REQUIREMENTS,
);
