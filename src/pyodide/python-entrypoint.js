// The entrypoint to a worker has to be an ES6 module (well, ignoring service
// workers). For Python workers, we use this file as the ES6 entrypoint.
//
// This file is treated as part of the user bundle and cannot import internal
// modules. Any imports from this file must be user-visible. To deal with this,
// we delegate the implementation to `python-entrypoint-helper` which is a
// BUILTIN module that can see our INTERNAL modules.

import {
  DurableObject,
  WorkerEntrypoint,
  WorkflowEntrypoint,
} from 'cloudflare:workers';

// The creation of `pythonDurableObjects` et al. has to be done here because
// python-entrypoint-helper is a BUILTIN and so cannot import `DurableObject` et al.
// (which are also builtins). As a workaround we call `makeEntrypointClass` here and pass it the
// appropriate class.
import {
  setDoAnImport,
  initPython,
  createImportProxy,
} from 'pyodide:python-entrypoint-helper';

// Function to dynamically import JavaScript modules from Python
// We need the import "call" to occur in this file since it is the only file that is part of the
// user bundle and can see BUILTIN modules and not INTERNAL modules. If we put the import in any
// other file, it would be possible to import INTERNAL modules (not good) and not possible to import
// USER or BUILTIN modules.
async function doAnImport(name) {
  const mod = await import(name);
  return createImportProxy(name, mod);
}

// Pass the import function to the helper
await setDoAnImport(doAnImport, WorkerEntrypoint);

// Initialise Python only after the import function has been set above.
const { handlers, pythonEntrypointClasses, makeEntrypointClass } =
  await initPython();

function makeEntrypointClassFromNames(classes, baseClass) {
  return classes.map(({ className, methodNames }) => [
    className,
    makeEntrypointClass(className, baseClass, methodNames),
  ]);
}

const entrypoints = {
  durableObjects: DurableObject,
  workerEntrypoints: WorkerEntrypoint,
  workflowEntrypoints: WorkflowEntrypoint,
};

const pythonEntrypoints = Object.fromEntries(
  Object.entries(entrypoints).flatMap(([key, baseClass]) => {
    const classes = pythonEntrypointClasses[key];
    return makeEntrypointClassFromNames(classes, baseClass);
  })
);

export { pythonEntrypoints };
export default 'default' in handlers ? handlers['default'] : handlers;
