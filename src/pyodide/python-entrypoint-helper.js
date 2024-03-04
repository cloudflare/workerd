// This file is a BUILTIN module that provides the actual implementation for the
// python-entrypoint.js USER module.

import { loadPyodide } from "pyodide-internal:python";
import { default as LOCKFILE } from "pyodide-internal:generated/pyodide-lock.json";
import { default as MetadataReader } from "pyodide-internal:runtime-generated/metadata";

const IS_WORKERD = MetadataReader.isWorkerd();
const WORKERD_INDEX_URL =
  "https://pub-45d734c4145d4285b343833ee450ef38.r2.dev/v1/";

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

async function setupPackages(pyodide) {
  patchLoadPackage(pyodide);

  const requirements = MetadataReader.getRequirements();
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
  const transitiveRequirements = new Set(packageDatas.map(({ name }) => name));

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

let mainModulePromise;
function getPyodide(ctx) {
  if (mainModulePromise !== undefined) {
    return mainModulePromise;
  }
  mainModulePromise = (async function () {
    // TODO: investigate whether it is possible to run part of loadPyodide in top level scope
    // When we do it in top level scope we seem to get a broken file system.
    const pyodide = await loadPyodide(ctx, LOCKFILE, WORKERD_INDEX_URL);
    await setupPackages(pyodide);
    const mainModule = pyimportMainModule(pyodide);
    return { mainModule };
  })();
  return mainModulePromise;
}

export default {
  async fetch(request, env, ctx) {
    try {
      const { mainModule } = await getPyodide(ctx);
      if (mainModule.on_fetch === undefined) {
        throw new Error("Python Worker should define an on_fetch method");
      }
      return await mainModule.on_fetch.callRelaxed(request, env, ctx);
    } catch(e) {
      console.warn(e.stack);
      throw e;
    }
  },
  async test(ctrl, env, ctx) {
    try {
      const { mainModule } = await getPyodide(ctx);
      return await mainModule.test.callRelaxed(ctrl, env, ctx);
    } catch (e) {
      console.warn(e.stack);
      throw e;
    }
  },
};
