import { loadPyodide } from "pyodide:python";
import worker from "./worker.py";

async function setupPyodide() {
  const pyodide = await loadPyodide();
  pyodide.FS.writeFile(`/session/worker.py`, new Uint8Array(worker), {
    canOwn: true,
  });
  const t2 = performance.now();
  return pyodide.pyimport("worker").fetch;
}

export default {
  async fetch(request) {
    const t1 = performance.now();
    const fetchHandler = await setupPyodide();
    const t2 = performance.now();
    const result = fetchHandler(request);
    const t3 = performance.now();
    console.log("bootstrap", t2 - t1);
    console.log("handle", t3 - t2);
    return result;
  },
  async test() {
    const fetchHandler = await setupPyodide();
    const result = fetchHandler(new Request(""));
    console.log(result);
  }
};
