// The entrypoint to a worker has to be an ES6 module (well, ignoring service
// workers). For Python workers, we use this file as the ES6 entrypoint.
//
// An important side effect of this is that this file is treated as part of the
// user bundle and cannot import internal modules. Any imports from this file
// must be user-visible. To deal with this, we keep most of the implementation
// to `python-entrypoint-helper` which is a BUILTIN module that can see our
// INTERNAL modules.

import { loadPyodide, setupPackages } from "pyodide:python-entrypoint-helper";
import { getMetadata } from "pyodide:current-bundle";

export default {
  async fetch(request, env) {
    const pyodide = await loadPyodide();
    const mainModule = await setupPackages(pyodide, getMetadata());
    return await mainModule.fetch(request);
  },
  async test() {
    try {
      const pyodide = await loadPyodide();
      const mainModule = await setupPackages(pyodide, getMetadata());
      return await mainModule.test();
    } catch (e) {
      console.warn(e);
    }
  },
};
