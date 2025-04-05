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
  pythonEntrypointClasses,
  makeEntrypointClass,
} from 'pyodide:python-entrypoint-helper';

function makeEntrypointClassFromNames(names, cls) {
  return names.map((className) => [
    className,
    makeEntrypointClass(className, cls),
  ]);
}

const pythonDurableObjects = makeEntrypointClassFromNames(
  pythonEntrypointClasses.durableObjects,
  DurableObject
);

const pythonWorkerEntrypoints = makeEntrypointClassFromNames(
  pythonEntrypointClasses.workerEntrypoints,
  WorkerEntrypoint
);

const pythonWorkflowEntrypoint = makeEntrypointClassFromNames(
  pythonEntrypointClasses.workflowEntrypoints,
  WorkflowEntrypoint
);

const pythonEntrypoints = Object.fromEntries([
  ...pythonDurableObjects,
  ...pythonWorkerEntrypoints,
  ...pythonWorkflowEntrypoint,
]);

export { pythonEntrypoints };
export { default } from 'pyodide:python-entrypoint-helper';
