// The entrypoint to a worker has to be an ES6 module (well, ignoring service
// workers). For Python workers, we use this file as the ES6 entrypoint.
//
// This file is treated as part of the user bundle and cannot import internal
// modules. Any imports from this file must be user-visible. To deal with this,
// we delegate the implementation to `python-entrypoint-helper` which is a
// BUILTIN module that can see our INTERNAL modules.

import { DurableObject } from 'cloudflare:workers';

// The creation of `pythonDurableObjects` has to be done here because python-entrypoint-helper
// is a BUILTIN and so cannot import `DurableObject` (which is also a builtin). As a workaround
// we call `makeDurableObjectClass` here and pass it the DurableObject class.
import {
  pythonDurableObjectClasses,
  makeDurableObjectClass,
} from 'pyodide:python-entrypoint-helper';

const pythonDurableObjects = Object.fromEntries(
  pythonDurableObjectClasses.map((className) => [
    className,
    makeDurableObjectClass(className, DurableObject),
  ])
);

export { pythonDurableObjects };
export { default } from 'pyodide:python-entrypoint-helper';
