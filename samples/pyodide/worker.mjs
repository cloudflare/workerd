import { loadPyodide } from "pyodide:python";
import worker from "./worker.py";

export default {
  async fetch(request) {
    const t1 = performance.now();
    const pyodide = await loadPyodide();

    pyodide.FS.writeFile(`/session/worker.py`, new Uint8Array(worker), {
      canOwn: true,
    });
    const t2 = performance.now();
    const result = await pyodide.pyimport("worker").onfetch(request);
    const t3 = performance.now();
    console.log("bootstrap", t2 - t1);
    console.log("handle", t3 - t2);
    return result;
  },
};
