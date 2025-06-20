/* eslint-disable @typescript-eslint/no-unsafe-argument, @typescript-eslint/no-unsafe-assignment, @typescript-eslint/no-unsafe-member-access, @typescript-eslint/no-unsafe-return  */
// This file is a BUILTIN module that provides the actual implementation for the
// python-entrypoint.js USER module.

import { loadPyodide } from 'pyodide-internal:python';
import { enterJaegerSpan } from 'pyodide-internal:jaeger';
import { patchLoadPackage } from 'pyodide-internal:setupPackages';
import {
  IS_TRACING,
  IS_WORKERD,
  LOCKFILE,
  TRANSITIVE_REQUIREMENTS,
  MAIN_MODULE_NAME,
  WORKERD_INDEX_URL,
  DURABLE_OBJECT_CLASSES,
  WORKER_ENTRYPOINT_CLASSES,
  SHOULD_SNAPSHOT_TO_DISK,
} from 'pyodide-internal:metadata';
import { default as Limiter } from 'pyodide-internal:limiter';
import { entropyBeforeRequest } from 'pyodide-internal:topLevelEntropy/lib';
import { reportError } from 'pyodide-internal:util';

// Function to import JavaScript modules from Python
const pyodide_entrypoint_helper: any = {};

export function setDoAnImport(
  func: (mod: string) => Promise<any>,
  cloudflareWorkers: any,
  cloudflareSockets: any
): void {
  pyodide_entrypoint_helper.doAnImport = func;
  pyodide_entrypoint_helper.cloudflareWorkersModule = cloudflareWorkers;
  pyodide_entrypoint_helper.cloudflareSocketsModule = cloudflareSockets;
}

async function pyimportMainModule(pyodide: Pyodide): Promise<PyModule> {
  if (!MAIN_MODULE_NAME.endsWith('.py')) {
    throw new Error('Main module needs to end with a .py file extension');
  }
  const mainModuleName = MAIN_MODULE_NAME.slice(0, -3);
  if (pyodide.version === '0.26.0a2') {
    return pyodide.pyimport(mainModuleName);
  } else {
    return await pyodide._module.API.pyodide_base.pyimport_impl.callPromising(
      mainModuleName
    );
  }
}

let pyodidePromise: Promise<Pyodide> | undefined;
async function getPyodide(): Promise<Pyodide> {
  return await enterJaegerSpan('get_pyodide', () => {
    if (pyodidePromise) {
      return pyodidePromise;
    }
    pyodidePromise = (async function (): Promise<Pyodide> {
      const pyodide = loadPyodide(IS_WORKERD, LOCKFILE, WORKERD_INDEX_URL);
      await setupPatches(pyodide);
      return pyodide;
    })();
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
    `${pyodide.FS.sitePackages}/${pyModName}.py`,
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
  await enterJaegerSpan('setup_patches', async () => {
    patchLoadPackage(pyodide);

    // install any extra packages into the site-packages directory
    const sitePackages = pyodide.FS.sitePackages;

    // Expose the doAnImport function and global modules to Python globals
    pyodide.registerJsModule(
      '_pyodide_entrypoint_helper',
      pyodide_entrypoint_helper
    );

    // Inject modules that enable JS features to be used idiomatically from Python.
    //
    // NOTE: setupPatches is called after memorySnapshotDoImports, so any modules injected here
    // shouldn't be part of the snapshot and should filtered out in filterPythonScriptImports.
    if (pyodide.version === '0.26.0a2') {
      // Inject at cloudflare.workers for backwards compatibility
      pyodide.FS.mkdir(`${sitePackages}/cloudflare`);
      await injectSitePackagesModule(pyodide, 'workers', 'cloudflare/workers');
    }
    // The SDK was moved from `cloudflare.workers` to just `workers`.
    await injectSitePackagesModule(pyodide, '_workers', '_workers');
    await injectSitePackagesModule(pyodide, 'workers', 'workers');

    // Install patches as needed
    if (TRANSITIVE_REQUIREMENTS.has('aiohttp')) {
      await applyPatch(pyodide, 'aiohttp');
    }
    // Other than the oldest version of httpx, we apply the patch at the build step.
    if (
      pyodide._module.API.version === '0.26.0a2' &&
      TRANSITIVE_REQUIREMENTS.has('httpx')
    ) {
      await applyPatch(pyodide, 'httpx');
    }
    await injectSitePackagesModule(pyodide, 'asgi', 'asgi');
  });
}

let mainModulePromise: Promise<PyModule> | undefined;
function getMainModule(): Promise<PyModule> {
  return enterJaegerSpan('get_main_module', async () => {
    if (mainModulePromise) {
      return mainModulePromise;
    }
    mainModulePromise = (async function (): Promise<PyModule> {
      const pyodide = await getPyodide();
      Limiter.beginStartup();
      try {
        return await enterJaegerSpan('pyimport_main_module', () =>
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
  try {
    const pyodide = await getPyodide();
    const mainModule = await getMainModule();
    entropyBeforeRequest(pyodide._module);
    return mainModule;
  } catch (e) {
    // In edgeworker test suite, without this we get the file name and line number of the exception
    // but no traceback. This gives us a full traceback.
    reportError(e as Error);
  }
}

function doPyCallHelper(
  relaxed: boolean,
  pyfunc: PyCallable,
  args: any[]
): any {
  if (pyfunc.callWithOptions) {
    return pyfunc.callWithOptions({ relaxed, promising: true }, ...args);
  }
  if (relaxed) {
    return pyfunc.callRelaxed(...args);
  }
  return pyfunc(...args);
}

function doPyCall(pyfunc: PyCallable, args: any[]): any {
  return doPyCallHelper(false, pyfunc, args);
}

function doRelaxedPyCall(pyfunc: PyCallable, args: any[]): any {
  return doPyCallHelper(true, pyfunc, args);
}

function makeHandler(pyHandlerName: string): Handler {
  if (pyHandlerName === 'test' && SHOULD_SNAPSHOT_TO_DISK) {
    return async function () {
      await getPyodide();
      console.log('Stored snapshot to disk; quitting without running test');
    };
  }
  return async function (...args: any[]) {
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
      return doRelaxedPyCall(handler, args);
    });

    // Support returning a pyodide.ffi.FetchResponse.
    return result?.js_response ?? result;
  };
}

async function initPyInstance(
  className: string,
  args: any[]
): Promise<PyModule> {
  const mainModule = await preparePython();
  const pyClassConstructor = mainModule[className];
  if (typeof pyClassConstructor !== 'function') {
    throw new TypeError(
      `There is no '${className}' class defined in the Python Worker's main module`
    );
  }
  const res = await doPyCall(pyClassConstructor, args);
  return res as PyModule;
}

// https://developers.cloudflare.com/workers/runtime-apis/rpc/reserved-methods/
const SPECIAL_HANDLER_NAMES = ['fetch', 'connect'];
const SPECIAL_DO_HANDLER_NAMES = [
  'alarm',
  'webSocketMessage',
  'webSocketClose',
  'webSocketError',
];

function makeEntrypointProxyHandler(
  pyInstancePromise: Promise<PyModule>,
  isDurableObject: boolean
): ProxyHandler<any> {
  return {
    get(target, prop, receiver): any {
      if (typeof prop !== 'string') {
        return Reflect.get(target, prop, receiver);
      }

      // Proxy calls to `fetch` to methods named `on_fetch` (and the same for other handlers.)
      const isKnownHandler = SPECIAL_HANDLER_NAMES.includes(prop);
      const isKnownDoHandler =
        isDurableObject && SPECIAL_DO_HANDLER_NAMES.includes(prop);
      const isFetch = prop == 'fetch';
      if (isKnownHandler || isKnownDoHandler) {
        prop = 'on_' + prop;
      }

      return async function (...args: any[]): Promise<any> {
        // Check if the requested method exists and if so, call it.
        const pyInstance = await pyInstancePromise;
        if (typeof pyInstance[prop] !== 'function') {
          throw new TypeError(`Method ${prop} does not exist`);
        }

        if ((isKnownHandler || isKnownDoHandler) && !isFetch) {
          return await doPyCallHelper(true, pyInstance[prop], args);
        }

        const introspectionMod = await getIntrospectionMod();

        return await doPyCall(introspectionMod.wrapper_func, [
          isFetch,
          pyInstance,
          prop,
          ...args,
        ]);
      };
    },
  };
}

function makeEntrypointClass(
  className: string,
  classKind: AnyClass,
  methods: string[]
): any {
  const isDurableObject = classKind.name === 'DurableObject';
  const result = class EntrypointWrapper extends classKind {
    constructor(...args: any[]) {
      super(...args);
      // Initialise a Python instance of the class.
      const pyInstancePromise = initPyInstance(className, args);
      // We do not know the methods that are defined on the RPC class, so we need a proxy to
      // support any possible method name.
      return new Proxy(
        this,
        makeEntrypointProxyHandler(pyInstancePromise, isDurableObject)
      );
    }
  };

  // Add dummy functions to the class so that the validator can detect them. These will never get
  // accessed because of the proxy at runtime.
  for (let method of methods) {
    if (SUPPORTED_HANDLER_NAMES.includes(method.slice(3))) {
      // Remove the "on_" prefix.
      method = method.slice(3);
    }
    result.prototype[method] = function (): void {};
  }
  return result;
}

type IntrospectionMod = {
  __dict__: PyDict;
  collect_entrypoint_classes: (mod: PyModule) => PythonEntrypointClasses;
  wrapper_func: PyCallable;
};

let introspectionModPromise: Promise<IntrospectionMod> | null = null;
async function loadIntrospectionMod(
  pyodide: Pyodide
): Promise<IntrospectionMod> {
  const introspectionSource = await import('pyodide-internal:introspection.py');
  const introspectionMod = pyodide.runPython(
    "from types import ModuleType; ModuleType('introspection')"
  ) as IntrospectionMod;
  const decoder = new TextDecoder();
  pyodide.runPython(decoder.decode(introspectionSource.default), {
    globals: introspectionMod.__dict__,
    filename: 'introspection.py',
  });

  return introspectionMod;
}

async function getIntrospectionMod(): Promise<IntrospectionMod> {
  if (introspectionModPromise === null) {
    introspectionModPromise = getPyodide().then(loadIntrospectionMod);
  }

  return introspectionModPromise;
}

const SUPPORTED_HANDLER_NAMES = [
  'fetch',
  'alarm',
  'scheduled',
  'trace',
  'queue',
  'pubsub',
];

type ExporterClassInfo = {
  className: string;
  methodNames: string[];
};

type PythonEntrypointClasses = {
  durableObjects: ExporterClassInfo[];
  workerEntrypoints: ExporterClassInfo[];
  workflowEntrypoints: ExporterClassInfo[];
};

type PythonInitResult = {
  handlers: { [handlerName: string]: Handler };
  pythonEntrypointClasses: PythonEntrypointClasses;
  makeEntrypointClass: typeof makeEntrypointClass;
};

export async function initPython(): Promise<PythonInitResult> {
  const handlers: {
    [handlerName: string]: Handler;
  } = {};

  let pythonEntrypointClasses: PythonEntrypointClasses = {
    durableObjects: [],
    workerEntrypoints: [],
    workflowEntrypoints: [],
  };

  // Do not setup anything to do with Python in the global scope when tracing. The Jaeger tracing
  // needs to be called inside an IO context.
  if (IS_TRACING) {
    // Currently when tracing we cannot perform IO in the
    // top-level. So we have some custom logic for handlers here in that case.
    //
    // Because of the above we have limited info, so we cannot get the method names of the classes
    // that are exported. But this doesn't matter as it's only useful for the validator.
    const toClassInfo = (x: string): ExporterClassInfo => {
      return { className: x, methodNames: [] };
    };
    pythonEntrypointClasses.durableObjects = (DURABLE_OBJECT_CLASSES ?? []).map(
      toClassInfo
    );
    // We currently have no way to discern between worker entrypoint classes and workflow entrypoint
    // classes in workerd. But workflow entrypoints appear to be just a special case of worker
    // entrypoints, so this should still work just fine.
    pythonEntrypointClasses.workerEntrypoints = (
      WORKER_ENTRYPOINT_CLASSES ?? []
    ).map(toClassInfo);

    for (const handlerName of SUPPORTED_HANDLER_NAMES) {
      const pyHandlerName = 'on_' + handlerName;
      handlers[handlerName] = makeHandler(pyHandlerName);
    }

    handlers.test = makeHandler('test');
  } else {
    const mainModule = await getMainModule();
    for (const handlerName of SUPPORTED_HANDLER_NAMES) {
      const pyHandlerName = 'on_' + handlerName;
      // We add all handlers when running in workerd, so that we can handle the case where the
      // handler is not defined in our own code and throw a more helpful error. See
      // undefined-handler.wd-test.
      if (typeof mainModule[pyHandlerName] === 'function' || IS_WORKERD) {
        handlers[handlerName] = makeHandler(pyHandlerName);
      }
    }

    if (typeof mainModule.test === 'function') {
      handlers.test = makeHandler('test');
    }

    // In order to get the entrypoint classes exported by the worker, we use a Python module
    // to introspect the user's main module. So we are effectively using Python to analyse the
    // classes exported by the user worker here. The class names are then exported from here and
    // used to create the equivalent JS classes via makeEntrypointClass.
    const introspectionMod = await getIntrospectionMod();
    pythonEntrypointClasses =
      introspectionMod.collect_entrypoint_classes(mainModule);
  }

  return { handlers, pythonEntrypointClasses, makeEntrypointClass };
}
