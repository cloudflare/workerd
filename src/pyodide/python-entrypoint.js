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
import { initPython } from 'pyodide:python-entrypoint-helper';

const { defaultExport, pythonEntrypoints } = await initPython({
  // Python imports JavaScript modules through this function. The import() has to happen in this
  // file: it is the only one in the user bundle, so it is the only one that can import USER and
  // BUILTIN modules and not INTERNAL ones.
  importModule: (name) => import(name),
  // python-entrypoint-helper is a BUILTIN, so it cannot import these (also BUILTIN) classes.
  baseClasses: {
    durableObjects: DurableObject,
    workerEntrypoints: WorkerEntrypoint,
    workflowEntrypoints: WorkflowEntrypoint,
  },
});

export { pythonEntrypoints };
export default defaultExport;
