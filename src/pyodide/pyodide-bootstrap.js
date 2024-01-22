import { loadPyodide, setupPackages } from "pyodide:pyodide-bootstrap-helper";
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
