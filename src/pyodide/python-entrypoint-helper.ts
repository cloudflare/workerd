// This file is a BUILTIN module that provides the actual implementation for the
// python-entrypoint.js USER module.

import { loadPyodide } from 'pyodide-internal:python';
import { enterJaegerSpan } from 'pyodide-internal:jaeger';
import {
  TRANSITIVE_REQUIREMENTS,
  patchLoadPackage,
} from 'pyodide-internal:setupPackages';
import {
  IS_TRACING,
  IS_WORKERD,
  LOCKFILE,
  MAIN_MODULE_NAME,
  WORKERD_INDEX_URL,
} from 'pyodide-internal:metadata';
import { reportError } from 'pyodide-internal:util';
import { default as Limiter } from 'pyodide-internal:limiter';
import { entropyBeforeRequest } from 'pyodide-internal:topLevelEntropy/lib';

function pyimportMainModule(pyodide: Pyodide): PyModule {
  if (!MAIN_MODULE_NAME.endsWith('.py')) {
    throw new Error('Main module needs to end with a .py file extension');
  }
  const mainModuleName = MAIN_MODULE_NAME.slice(0, -3);
  return pyodide.pyimport(mainModuleName);
}

let pyodidePromise: Promise<Pyodide> | undefined;
function getPyodide(): Promise<Pyodide> {
  return enterJaegerSpan('get_pyodide', () => {
    if (pyodidePromise) {
      return pyodidePromise;
    }
    pyodidePromise = loadPyodide(IS_WORKERD, LOCKFILE, WORKERD_INDEX_URL);
    return pyodidePromise;
  });
}

/**
 * Import the data from the data module es6 import called jsModName.py into a module called
 * pyModName.py. The site_packages directory is on the path.
 */
async function injectSitePackagesModule(
  pyodide: Pyodide,
  jsModName: string,
  pyModName: string
): Promise<void> {
  const mod = await import(`pyodide-internal:${jsModName}.py`);
  pyodide.FS.writeFile(
    `${pyodide.site_packages}/${pyModName}.py`,
    new Uint8Array(mod.default),
    { canOwn: true }
  );
}

/**
 * Put the patch into site_packages and import it.
 *
 * TODO: Ideally we should only import the patch lazily when the package that it patches is
 * imported. Or just apply the patch directly or upstream a fix.
 */
async function applyPatch(pyodide: Pyodide, patchName: string): Promise<void> {
  await injectSitePackagesModule(
    pyodide,
    `patches/${patchName}`,
    patchName + '_patch'
  );
  pyodide.pyimport(patchName + '_patch');
}

async function setupPatches(pyodide: Pyodide): Promise<void> {
  return await enterJaegerSpan('setup_patches', async () => {
    patchLoadPackage(pyodide);

    // install any extra packages into the site-packages directory, so calculate where that is.
    const pymajor = pyodide._module._py_version_major();
    const pyminor = pyodide._module._py_version_minor();
    pyodide.site_packages = `/lib/python${pymajor}.${pyminor}/site-packages`;

    // Inject modules that enable JS features to be used idiomatically from Python.
    pyodide.FS.mkdir(`${pyodide.site_packages}/cloudflare`);
    await injectSitePackagesModule(pyodide, 'workers', 'cloudflare/workers');

    // Install patches as needed
    if (TRANSITIVE_REQUIREMENTS.has('aiohttp')) {
      await applyPatch(pyodide, 'aiohttp');
    }
    if (TRANSITIVE_REQUIREMENTS.has('httpx')) {
      await applyPatch(pyodide, 'httpx');
    }
    if (TRANSITIVE_REQUIREMENTS.has('fastapi')) {
      await injectSitePackagesModule(pyodide, 'asgi', 'asgi');
    }
  });
}

let mainModulePromise: Promise<PyModule> | undefined;
function getMainModule(): Promise<PyModule> {
  return enterJaegerSpan('get_main_module', async () => {
    if (mainModulePromise) {
      return mainModulePromise;
    }
    mainModulePromise = (async function () {
      const pyodide = await getPyodide();
      await setupPatches(pyodide);
      Limiter.beginStartup();
      try {
        return enterJaegerSpan('pyimport_main_module', () =>
          pyimportMainModule(pyodide)
        );
      } finally {
        Limiter.finishStartup();
      }
    })();
    return mainModulePromise;
  });
}

async function preparePython(): Promise<PyModule> {
  const pyodide = await getPyodide();
  const mainModule = await getMainModule();
  entropyBeforeRequest(pyodide._module);
  return mainModule;
}

function makeHandler(pyHandlerName: string): Handler {
  return async function (...args: any[]) {
    try {
      const mainModule = await enterJaegerSpan(
        'prep_python',
        async () => await preparePython()
      );
      const handler = mainModule[pyHandlerName];
      if (!handler) {
        throw new Error(
          `Python entrypoint "${MAIN_MODULE_NAME}" does not export a handler named "${pyHandlerName}"`
        );
      }
      const result = await enterJaegerSpan('python_code', () => {
        return handler.callRelaxed(...args);
      });

      // Support returning a pyodide.ffi.FetchResponse.
      if (result && result.js_response !== undefined) {
        return result.js_response;
      } else {
        return result;
      }
    } catch (e) {
      console.warn('Error in makeHandler');
      reportError(e);
    }
  };
}
const handlers: {
  [handlerName: string]: Handler;
} = {};

try {
  // Do not setup anything to do with Python in the global scope when tracing. The Jaeger tracing
  // needs to be called inside an IO context.
  if (IS_WORKERD || IS_TRACING) {
    handlers.fetch = makeHandler('on_fetch');
    handlers.test = makeHandler('test');
  } else {
    const mainModule = await getMainModule();
    for (const handlerName of [
      'fetch',
      'alarm',
      'scheduled',
      'trace',
      'queue',
      'pubsub',
    ]) {
      const pyHandlerName = 'on_' + handlerName;
      if (typeof mainModule[pyHandlerName] === 'function') {
        handlers[handlerName] = makeHandler(pyHandlerName);
      }
    }
  }
} catch (e) {
  console.warn('Error in top level in python-entrypoint-helper.js');
  reportError(e);
}

export default handlers;
