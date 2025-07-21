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
import * as cloudflareWorkersModule from 'cloudflare:workers';
import * as cloudflareSocketsModule from 'cloudflare:sockets';

// The creation of `pythonDurableObjects` et al. has to be done here because
// python-entrypoint-helper is a BUILTIN and so cannot import `DurableObject` et al.
// (which are also builtins). As a workaround we call `makeEntrypointClass` here and pass it the
// appropriate class.
import { setDoAnImport, initPython } from 'pyodide:python-entrypoint-helper';

// Function to dynamically import JavaScript modules from Python
async function doAnImport(name) {
  return await import(name);
}

// Pass the import function to the helper
setDoAnImport(
  doAnImport,
  cloudflareWorkersModule,
  cloudflareSocketsModule,
  WorkerEntrypoint
);

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
