/**
 * This file is a simplified version of the Pyodide loader:
 * https://github.com/pyodide/pyodide/blob/main/src/js/pyodide.ts
 *
 * In particular, it drops the package lock, which disables
 * `pyodide.loadPackage`. In trade we add memory snapshots here.
 */


/**
 * _createPyodideModule and pyodideWasmModule together are produced by the
 * Emscripten linker
 */
import { _createPyodideModule } from "pyodide-internal:pyodide-bundle/pyodide.asm";
import pyodideWasmModule from "pyodide-internal:pyodide-bundle/pyodide.asm.wasm";


/**
 * The Python and Pyodide stdlib zipped together. The zip format is convenient
 * because Python has a "ziploader" that allows one to import directly from a
 * zip file.
 *
 * The ziploader solves bootstrapping problems around unpacking: Python comes
 * with a bunch of C libs to unpack various archive formats, but they need stuff
 * in this zip file to initialize their runtime state.
 */
import stdlib from "pyodide-internal:pyodide-bundle/python_stdlib.zip";

/**
 * Global variable for the memory snapshot. On the first run we stick a copy of
 * the heap here, on subsequent runs we can skip bootstrapping Python which is
 * quite slow. Startup with snapshot is 3-5 times faster than without it.
 *
 * In the future we could consider creating a memory snapshot ahead of time so
 * that latency is more predictable.
 *
 * Unfortunately this memory snapshot can easily get to ~30mb depending on the
 * script. The peak memory usage for Python seems to be reached at startup when
 * compiling Python source code, so if we use pre-compiled Python code the heap
 * is much smaller. Unfortunately, in recent versions of Python, pyc has large
 * inline caches which bloats the size of pyc files by a lot. A special-built
 * compressor should be able to drop the inline cache space and then calculate
 * how much space to insert from the opcode metadata.
 *
 * Possibly we could make many scripts share the same memory snapshot to get
 * decent performance/memory usage tradeoff.
 */
let memory = undefined;

/**
 * Much simpified version of the `prepareFileSystem` function here:
 * https://github.com/pyodide/pyodide/blob/main/src/js/module.ts
 */
function prepareFileSystem(Module) {
  const pymajor = 3;
  const pyminor = 11;
  Module.FS.mkdirTree("/lib");
  Module.FS.mkdirTree(`/lib/python${pymajor}.${pyminor}/site-packages`);
  Module.FS.writeFile(
    `/lib/python${pymajor}${pyminor}.zip`,
    new Uint8Array(stdlib),
    { canOwn: true }
  );
  Module.FS.mkdir(Module.API.config.env.HOME);
}

async function makeSnapshot(Module, run) {
  // This is the list of all packages imported by the Python bootstrap. We don't
  // want to spend time initializing these packages, so we make sure here that
  // the heap snapshot has them already initialized.
  // Can get this list by starting Python and filtering sys.modules for modules
  // whose importer is not FrozenImporter or BuiltinImporter.
  const imports = [
    "_pyodide.docstring",
    "_pyodide._core_docs",
    "traceback",
    "collections.abc",
    // Asyncio is the really slow one here. In native Python3.11 on my machine,
    // `import asyncio` takes ~50 ms.
    "asyncio",
    "inspect",
    "tarfile",
    "importlib.metadata",
    "re",
    "shutil",
    "sysconfig",
    "importlib.machinery",
    "pathlib",
    "site",
    "tempfile",
    "typing",
    "zipfile",
  ];
  const to_import = imports.join(",");
  const to_delete = Array.from(
    new Set(imports.map((x) => x.split(".")[0]))
  ).join(",");
  run(`import ${to_import}`);
  run("sysconfig.get_config_vars()");
  // Delete to avoid polluting top level namespace
  run(`del ${to_delete}`);
  // store a copy
  memory = Module.HEAP8.slice();
}

export async function loadPyodide() {
  const internalAPI = {};
  const config = { jsglobals: globalThis, env: { HOME: "/session" } };
  internalAPI.config = config;
  // Settings to control runtime instantiation.
  const emscriptenModule = {
    // skip running main() if we have a snapshot
    noInitialRun: !!memory,
    locateFile: (x) => x,
    instantiateWasm(info, receiveInstance) {
      (async function () {
        // By default Emscripten would use WebAssembly.instantiateStreaming, but
        // that's not allowed. We've compiled the wasmModule ahead of time anyways.
        const instance = await WebAssembly.instantiate(pyodideWasmModule, info);
        receiveInstance(instance, pyodideWasmModule);
      })();
      // This callback is required to either compile synchronously and return
      // the instance and pyodideWasmModule or return an empty object to
      // indicate that it's an async function. In the async case the result
      // should be passed to `receiveInstance`.
      return {};
    },
    preRun: [prepareFileSystem],
    API: internalAPI,
  };

  try {
    // Force Emscripten to feature detect the way we want
    // They used to have an `environment` setting that did this but it has been
    // removed =(
    // TODO: consider patching this in patch_pyodide_js.py instead.
    globalThis.window = {};       // makes ENVIRONMENT_IS_WEB    = true
    globalThis.importScripts = 1; // makes ENVIRONMENT_IS_WORKER = false
    const p = _createPyodideModule(emscriptenModule);
    delete globalThis.window;
    delete globalThis.importScripts;
    await p;
  } catch (e) {
    // Wish workerd has better error logging...
    e.stack.split("\n").forEach(console.log.bind(console));
  }
  function run(code) {
    const [status, err] = internalAPI.rawRun(code);
    if (status) {
      console.warn("Command failed:", code);
      console.warn("Error was:");
      for (const line of err.split("\n")) {
        console.warn(line);
      }
      throw new Error("Failed");
    }
  }

  if (!memory) {
    await makeSnapshot(emscriptenModule, run);
  } else {
    emscriptenModule.growMemory(memory.byteLength);
    emscriptenModule.HEAP8.set(new Uint8Array(memory));
  }

  let [err, captured_stderr] = internalAPI.rawRun("import _pyodide_core");
  if (err) {
    internalAPI.fatal_loading_error(
      "Failed to import _pyodide_core\n",
      captured_stderr
    );
  }
  internalAPI.finalizeBootstrap();
  return internalAPI.public_api;
}
