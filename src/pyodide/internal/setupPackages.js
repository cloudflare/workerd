import { parseTarInfo } from "pyodide-internal:tar";
import { createTarFS } from "pyodide-internal:tarfs";
import { createMetadataFS } from "pyodide-internal:metadatafs";
import { default as LOCKFILE } from "pyodide-internal:generated/pyodide-lock.json";
import { IS_WORKERD, REQUIREMENTS } from "pyodide-internal:metadata";

const canonicalizeNameRegex = /[-_.]+/g;

/**
 * Canonicalize a package name. Port of Python's packaging.utils.canonicalize_name.
 * @param name The package name to canonicalize.
 * @returns The canonicalize package name.
 * @private
 */
export function canonicalizePackageName(name) {
  return name.replace(canonicalizeNameRegex, "-").toLowerCase();
}

// The "name" field in the lockfile is not canonicalized
const STDLIB_PACKAGES = Object.values(LOCKFILE.packages)
  .filter(({ install_dir }) => install_dir === "stdlib")
  .map(({ name }) => canonicalizePackageName(name));

export function buildSitePackages(requirements) {
  const [origTarInfo, _] = parseTarInfo();
  if (IS_WORKERD) {
    return origTarInfo;
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

  for (const req of new Set([...STDLIB_PACKAGES, ...requirements])) {
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

  return newTarInfo;
}

/**
 * Patch loadPackage:
 *  - in workerd, disable integrity checks
 *  - otherwise, disable it entirely
 */
export function patchLoadPackage(pyodide) {
  if (!IS_WORKERD) {
    pyodide.loadPackage = disabledLoadPackage;
    return;
  }
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

export function getTransitiveRequirements() {
  const requirements = REQUIREMENTS.map(canonicalizePackageName);
  // resolve transitive dependencies of requirements and if IN_WORKERD install them from the cdn.
  const packageDatas = recursiveDependencies(LOCKFILE, requirements);
  return new Set(packageDatas.map(({ name }) => canonicalizePackageName(name)));
}

export function mountLib(pyodide, info) {
  const tarFS = createTarFS(pyodide._module);
  const mdFS = createMetadataFS(pyodide._module);
  const pymajor = pyodide._module._py_version_major();
  const pyminor = pyodide._module._py_version_minor();
  const site_packages = `/session/lib/python${pymajor}.${pyminor}/site-packages`;
  pyodide.FS.mkdirTree(site_packages);
  pyodide.FS.mkdirTree("/session/metadata");
  pyodide.FS.mount(tarFS, { info }, site_packages);
  pyodide.FS.mount(mdFS, {}, "/session/metadata");
  const sys = pyodide.pyimport("sys");
  sys.path.push(site_packages);
  sys.path.push("/session/metadata");
  sys.destroy();
}

export function recursiveDependencies(lockfile, names) {
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
