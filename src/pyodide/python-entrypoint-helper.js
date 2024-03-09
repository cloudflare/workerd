// This file is a BUILTIN module that provides the actual implementation for the
// python-entrypoint.js USER module.

import {
  loadPyodide,
  mountLib,
  canonicalizePackageName,
} from "pyodide-internal:python";
import { default as LOCKFILE } from "pyodide-internal:generated/pyodide-lock.json";
import { default as MetadataReader } from "pyodide-internal:runtime-generated/metadata";
import { default as PYODIDE_BUCKET } from "pyodide-internal:generated/pyodide-bucket.json";

const IS_WORKERD = MetadataReader.isWorkerd();
const WORKERD_INDEX_URL = PYODIDE_BUCKET.PYODIDE_PACKAGE_BUCKET_URL;

/**
 * Import the data from the data module es6 import called jsModName.py into a module called
 * pyModName.py. The site_packages directory is on the path.
 */
async function injectSitePackagesModule(pyodide, jsModName, pyModName) {
  const mod = await import(`pyodide-internal:${jsModName}.py`);
  pyodide.FS.writeFile(
    `${pyodide.site_packages}/${pyModName}.py`,
    new Uint8Array(mod.default),
    { canOwn: true },
  );
}

/**
 * Put the patch into site_packages and import it.
 *
 * TODO: Ideally we should only import the patch lazily when the package that it patches is
 * imported. Or just apply the patch directly or upstream a fix.
 */
async function applyPatch(pyodide, patchName) {
  await injectSitePackagesModule(
    pyodide,
    `patches/${patchName}`,
    patchName + "_patch",
  );
  pyodide.pyimport(patchName + "_patch");
}

/**
 * Patch loadPackage:
 *  - in workerd, disable integrity checks
 *  - otherwise, disable it entirely
 */
function patchLoadPackage(pyodide) {
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

async function getTransitiveRequirements(pyodide, requirements) {
  // resolve transitive dependencies of requirements and if IN_WORKERD install them from the cdn.
  let packageDatas;
  if (IS_WORKERD) {
    packageDatas = await pyodide.loadPackage(requirements);
  } else {
    // TODO: if not IS_WORKERD, use packageDatas to control package visibility.
    packageDatas = pyodide._api
      .recursiveDependencies(requirements)
      .values()
      .map(({ packageData }) => packageData);
  }
  return new Set(packageDatas.map(({ name }) => name));
}

async function setupPackages(pyodide) {
  patchLoadPackage(pyodide);
  const requirements = MetadataReader.getRequirements().map(
    canonicalizePackageName,
  );
  const transitiveRequirements = new Set(
    Array.from(await getTransitiveRequirements(pyodide, requirements)).map(
      canonicalizePackageName,
    ),
  );

  mountLib(pyodide, transitiveRequirements, IS_WORKERD);

  // install any extra packages into the site-packages directory, so calculate where that is.
  const pymajor = pyodide._module._py_version_major();
  const pyminor = pyodide._module._py_version_minor();
  pyodide.site_packages = `/lib/python${pymajor}.${pyminor}/site-packages`;

  // Install patches as needed
  if (transitiveRequirements.has("aiohttp")) {
    await applyPatch(pyodide, "aiohttp");
  }
  if (transitiveRequirements.has("httpx")) {
    await applyPatch(pyodide, "httpx");
  }
  if (transitiveRequirements.has("fastapi")) {
    await injectSitePackagesModule(pyodide, "asgi", "asgi");
  }
}

function pyimportMainModule(pyodide) {
  let mainModuleName = MetadataReader.getMainModule();
  if (!mainModuleName.endsWith(".py")) {
    throw new Error("Main module needs to end with a .py file extension");
  }
  mainModuleName = mainModuleName.slice(0, -3);
  return pyodide.pyimport(mainModuleName);
}

let pyodidePromise;
function getPyodide() {
  if (pyodidePromise) {
    return pyodidePromise;
  }
  pyodidePromise = loadPyodide(LOCKFILE, WORKERD_INDEX_URL);
  return pyodidePromise;
}

let mainModulePromise;
function getMainModule() {
  if (mainModulePromise) {
    return mainModulePromise;
  }
  mainModulePromise = (async function () {
    const pyodide = await getPyodide();
    await setupPackages(pyodide);
    return pyimportMainModule(pyodide);
  })();
  return mainModulePromise;
}

if (IS_WORKERD) {
  // If we're in workerd, we have to do setupPackages in the IoContext, so don't start it yet.
  // TODO: fix this.
  await getPyodide();
} else {
  // If we're not in workerd, setupPackages doesn't require IO so we can do it all here.
  await getMainModule();
}

/**
 * Have to reseed randomness in the IoContext of the first request since we gave a low quality seed
 * when it was seeded at top level.
 */
let isSeeded = false;
function reseedRandom(pyodide) {
  if (isSeeded) {
    return;
  }
  isSeeded = true;
  pyodide.runPython(`
    from random import seed
    seed()
    del seed
  `);
}

async function preparePython() {
  const pyodide = await getPyodide();
  reseedRandom(pyodide);
  return await getMainModule();
}

function makeHandler(pyHandlerName) {
  return async function (...args) {
    try {
      const mainModule = await preparePython();
      return await mainModule[pyHandlerName].callRelaxed(...args);
    } catch (e) {
      console.warn(e.stack);
      throw e;
    }
  };
}

const handlers = {};

if (IS_WORKERD) {
  handlers.fetch = makeHandler("on_fetch");
  handlers.test = makeHandler("test");
} else {
  const mainModule = await getMainModule();
  for (const handlerName of [
    "fetch",
    "alarm",
    "scheduled",
    "trace",
    "queue",
    "pubsub",
  ]) {
    const pyHandlerName = "on_" + handlerName;
    if (typeof mainModule[pyHandlerName] === "function") {
      handlers[handlerName] = makeHandler(pyHandlerName);
    }
  }
}

export default handlers;
