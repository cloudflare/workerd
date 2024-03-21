// This file is a BUILTIN module that provides the actual implementation for the
// python-entrypoint.js USER module.

import {
  loadPyodide,
  uploadArtifacts,
  getMemoryToUpload,
} from "pyodide-internal:python";
import { enterJaegerSpan } from "pyodide-internal:jaeger";
import {
  REQUIREMENTS,
  TRANSITIVE_REQUIREMENTS,
  USE_LOAD_PACKAGE,
  patchLoadPackage,
} from "pyodide-internal:setupPackages";
import {
  IS_TRACING,
  IS_WORKERD,
  LOCKFILE,
  MAIN_MODULE_NAME,
  WORKERD_INDEX_URL,
} from "pyodide-internal:metadata";
import { default as ArtifactBundler } from "pyodide-internal:artifacts";

function pyimportMainModule(pyodide) {
  if (!MAIN_MODULE_NAME.endsWith(".py")) {
    throw new Error("Main module needs to end with a .py file extension");
  }
  const mainModuleName = MAIN_MODULE_NAME.slice(0, -3);
  return pyodide.pyimport(mainModuleName);
}

let pyodidePromise;
function getPyodide() {
  return enterJaegerSpan("get_pyodide", () => {
    if (pyodidePromise) {
      return pyodidePromise;
    }
    pyodidePromise = loadPyodide(LOCKFILE, WORKERD_INDEX_URL);
    return pyodidePromise;
  });
}

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
 * Set up Python packages:
 *  - patch loadPackage to ignore integrity
 *  - get requirements
 *  - Use tar file + requirements to mount site packages directory
 *  - if in workerd use loadPackage to load packages
 *  - install patches to make various requests packages work
 *
 * TODO: move this into setupPackages.js. Can't now because the patch imports
 * fail from there for some reason.
 */
export async function setupPackages(pyodide) {
  return await enterJaegerSpan("setup_packages", async () => {
    patchLoadPackage(pyodide);
    if (USE_LOAD_PACKAGE) {
      await pyodide.loadPackage(REQUIREMENTS);
    }
    // install any extra packages into the site-packages directory, so calculate where that is.
    const pymajor = pyodide._module._py_version_major();
    const pyminor = pyodide._module._py_version_minor();
    pyodide.site_packages = `/lib/python${pymajor}.${pyminor}/site-packages`;

    // Install patches as needed
    if (TRANSITIVE_REQUIREMENTS.has("aiohttp")) {
      await applyPatch(pyodide, "aiohttp");
    }
    if (TRANSITIVE_REQUIREMENTS.has("httpx")) {
      await applyPatch(pyodide, "httpx");
    }
    if (TRANSITIVE_REQUIREMENTS.has("fastapi")) {
      await injectSitePackagesModule(pyodide, "asgi", "asgi");
    }
  });
}

let mainModulePromise;
function getMainModule() {
  return enterJaegerSpan("get_main_module", async () => {
    if (mainModulePromise) {
      return mainModulePromise;
    }
    mainModulePromise = (async function () {
      const pyodide = await getPyodide();
      await setupPackages(pyodide);
      return enterJaegerSpan("pyimport_main_module", () =>
        pyimportMainModule(pyodide),
      );
    })();
    return mainModulePromise;
  });
}

// Do not setup anything to do with Python in the global scope when tracing. The Jaeger tracing
// needs to be called inside an IO context.
if (!IS_TRACING) {
  if (IS_WORKERD) {
    // If we're in workerd, we have to do setupPackages in the IoContext, so don't start it yet.
    // TODO: fix this.
    await getPyodide();
  } else {
    // If we're not in workerd, setupPackages doesn't require IO so we can do it all here.
    await getMainModule();
  }
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
      const mainModule = await enterJaegerSpan(
        "prep_python",
        async () => await preparePython(),
      );
      return await enterJaegerSpan("python_code", () => {
        return mainModule[pyHandlerName].callRelaxed(...args);
      });
    } catch (e) {
      console.warn(e.stack);
      throw e;
    } finally {
      args[2].waitUntil(uploadArtifacts());
    }
  };
}

const handlers = {};

if (IS_WORKERD || IS_TRACING) {
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

// Store the memory snapshot in the ArtifactBundler so that the validator can read it out.
// Needs to happen at the top level because the validator does not perform requests.
if (ArtifactBundler.isEwValidating()) {
  ArtifactBundler.storeMemorySnapshot(getMemoryToUpload());
}

export default handlers;
