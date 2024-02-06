import { parseTarInfo } from "pyodide-internal:tar";
import { createTarFS } from "pyodide-internal:tarfs";
import { createMetadataFS } from "pyodide-internal:metadatafs";

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
import { _createPyodideModule } from "pyodide-internal:generated/pyodide.asm";
import pyodideWasmModule from "pyodide-internal:generated/pyodide.asm.wasm";

/**
 * The Python and Pyodide stdlib zipped together. The zip format is convenient
 * because Python has a "ziploader" that allows one to import directly from a
 * zip file.
 *
 * The ziploader solves bootstrapping problems around unpacking: Python comes
 * with a bunch of C libs to unpack various archive formats, but they need stuff
 * in this zip file to initialize their runtime state.
 */
import stdlib from "pyodide-internal:generated/python_stdlib.zip";

/**
 * Global variable for the memory snapshot. On the first run we stick a copy of
 * the linear memory here, on subsequent runs we can skip bootstrapping Python
 * which is quite slow. Startup with snapshot is 3-5 times faster than without
 * it.
 *
 * In the future we could consider creating a memory snapshot ahead of time so
 * that latency is more predictable.
 *
 * Unfortunately this memory snapshot can easily get to ~30mb depending on the
 * script. The peak memory usage for Python seems to be reached at startup when
 * compiling Python source code, so if we use pre-compiled Python code the wasm
 * linear memory is much smaller. Unfortunately, in recent versions of Python,
 * pyc has large inline caches which bloats the size of pyc files by a lot. A
 * special-built compressor should be able to drop the inline cache space and
 * then calculate how much space to insert from the opcode metadata.
 *
 * Possibly we could make many scripts share the same memory snapshot to get
 * decent performance/memory usage tradeoff.
 */
let MEMORY = undefined;

/**
 * This is passed as a preRun hook in EmscriptenSettings, run just before
 * main(). It ensures that the file system includes the stuff that main() needs,
 * most importantly the Python standard library.
 *
 * Put the Python + Pyodide standard libraries into a zip file in the
 * appropriate location /lib/python311.zip . Python will import stuff directly
 * from this zip file using ZipImporter.
 *
 * ZipImporter is quite useful here -- the Python runtime knows how to unpack a
 * bunch of different archive formats but it is not possible to use these until
 * the runtime state is initialized. So ZipImporter breaks this bootstrapping
 * knot for us.
 *
 * We also make an empty home directory and an empty global site-packages
 * directory `/lib/python3.11/site-packages`.
 *
 * This is a simpified version of the `prepareFileSystem` function here:
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

/**
 * A hook that the Emscripten runtime calls to perform the WebAssembly
 * instantiation action. Once instantiated, this callback function should call
 * ``successCallback()`` with the generated WebAssembly Instance object.
 *
 * @param wasmImports a JS object which contains all the function imports that
 * need to be passed to the WebAssembly Module when instantiating
 * @param successCallback A callback to indicate that instantiation was
 * successful,
 * @returns The return value of this function should contain the ``exports`` object of
 * the instantiated WebAssembly Module, or an empty dictionary object ``{}`` if
 * the instantiation is performed asynchronously, or ``false`` if instantiation
 * synchronously failed. There is no way to indicate asynchronous failure.
 */
function instantiateWasm(wasmImports, successCallback) {
  (async function () {
    // Instantiate pyodideWasmModule with wasmImports
    const instance = await WebAssembly.instantiate(
      pyodideWasmModule,
      wasmImports
    );
    successCallback(instance, pyodideWasmModule);
  })();

  return {};
}

/**
 * The Emscripten settings object
 */
function getEmscriptenSettings() {
  const config = { jsglobals: globalThis, env: { HOME: "/session" } };
  const API = { config };
  // Emscripten settings to control runtime instantiation.
  return {
    // preRun hook to set up the file system before running main
    // The preRun hook gets run independently of noInitialRun, which is
    // important because the file system lives outside of linear memory.
    preRun: [prepareFileSystem],
    instantiateWasm,
    noInitialRun: !!MEMORY, // skip running main() if we have a snapshot
    API, // Pyodide requires we pass this in.
  };
}

/**
 * Simple wrapper around _createPyodideModule that applies some monkey patches
 * to force the environment to be detected the way we want.
 *
 * In the long run we should fix this in `pyodide.asm.js` instead.
 *
 * Returns the instantiated emscriptenModule object.
 */
async function instantiateEmscriptenModule(emscriptenSettings) {
  try {
    // Force Emscripten to feature detect the way we want
    // They used to have an `environment` setting that did this but it has been
    // removed =(
    // TODO: consider patching this in patch_pyodide_js.bzl instead.
    globalThis.window = {}; // makes ENVIRONMENT_IS_WEB    = true
    globalThis.importScripts = 1; // makes ENVIRONMENT_IS_WORKER = false
    const p = _createPyodideModule(emscriptenSettings);
    delete globalThis.window;
    delete globalThis.importScripts;
    const emscriptenModule = await p;
    return emscriptenModule;
  } catch (e) {
    // Wish workerd has better error logging...
    e.stack.split("\n").forEach(console.log.bind(console));
    throw e;
  }
}

/**
 * After running `instantiateEmscriptenModule` but before calling into any C
 * APIs, we call this function. If `MEMORY` is defined, then we will have passed
 * `noInitialRun: true` and so the C runtime is in an incoherent state until we
 * restore the linear memory from the snapshot.
 */
async function prepareWasmLinearMemory(emscriptenModule) {
  if (MEMORY) {
    // resize linear memory to fit our snapshot. I think `growMemory` only
    // exists if `-sALLOW_MEMORY_GROWTH` is passed to the linker but we'll
    // probably always do that.
    emscriptenModule.growMemory(MEMORY.byteLength);
    // restore memory from snapshot
    emscriptenModule.HEAP8.set(new Uint8Array(MEMORY));
  } else {
    await makeLinearMemorySnapshot(emscriptenModule);
  }
}

// This is the list of all packages imported by the Python bootstrap. We don't
// want to spend time initializing these packages, so we make sure here that
// the linear memory snapshot has them already initialized.
// Can get this list by starting Python and filtering sys.modules for modules
// whose importer is not FrozenImporter or BuiltinImporter.
const SNAPSHOT_IMPORTS = [
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

/**
 * Create memory snapshot by importing SNAPSHOT_IMPORTS to ensure these packages
 * are initialized in the linear memory snapshot and then saving a copy of the
 * linear memory into MEMORY.
 */
async function makeLinearMemorySnapshot(emscriptenModule) {
  const toImport = SNAPSHOT_IMPORTS.join(",");
  const toDelete = Array.from(
    new Set(SNAPSHOT_IMPORTS.map((x) => x.split(".")[0]))
  ).join(",");
  simpleRunPython(emscriptenModule, `import ${toImport}`);
  simpleRunPython(emscriptenModule, "sysconfig.get_config_vars()");
  // Delete to avoid polluting globals
  simpleRunPython(emscriptenModule, `del ${toDelete}`);
  // store a copy of wasm linear memory into our MEMORY global variable.
  MEMORY = emscriptenModule.HEAP8.slice();
}

/**
 *  Simple as possible runPython function which works with essentially no
 *  foreign function interface. We need to use this rather than the normal
 *  easier to use interface because the normal interface doesn't work until
 *  after `API.finalizeBootstrap`, but `API.finalizeBootstrap` makes changes
 *  inside and outside the linear memory which have to stay in sync. It's hard
 *  to keep track of the invariants that `finalizeBootstrap` introduces between
 *  JS land and the linear memory so we do this.
 *
 *  We wrap API.rawRun which does the following steps:
 *  1. use textEncoder.encode to convert `code` into UTF8 bytes
 *  2. malloc space for `code` in the wasm linear memory and copy the encoded
 *      `code` to this pointer
 *  3. redirect standard error to a temporary buffer
 *  4. call `PyRun_SimpleString`, which either works and returns 0 or formats a
 *      traceback to stderr and returns -1
 *      https://docs.python.org/3/c-api/veryhigh.html?highlight=simplestring#c.PyRun_SimpleString
 *  5. frees the `code` pointer
 *  6. Returns the return value from `PyRun_SimpleString` and whatever
 *      information went to stderr.
 *
 *  PyRun_SimpleString executes the code at top level in the `__main__` module,
 *  so all variables defined get leaked into the global namespace unless we
 *  clean them up explicitly.
 */
function simpleRunPython(emscriptenModule, code) {
  const [status, err] = emscriptenModule.API.rawRun(code);
  // status 0: Ok
  // status -1: Error
  if (status) {
    // PyRun_SimpleString will have written a Python traceback to stderr.
    console.warn("Command failed:", code);
    console.warn("Error was:");
    for (const line of err.split("\n")) {
      console.warn(line);
    }
    throw new Error("Failed");
  }
}

function mountLib(pyodide) {
  const [info, _] = parseTarInfo();
  const tarFS = createTarFS(pyodide._module);
  const mdFS = createMetadataFS(pyodide._module);
  pyodide.FS.mkdirTree("/session/lib/python3.11/site-packages");
  pyodide.FS.mkdirTree("/session/metadata");
  pyodide.FS.mount(tarFS, { info }, "/session/lib/python3.11/site-packages");
  pyodide.FS.mount(mdFS, {}, "/session/metadata");
  const sys = pyodide.pyimport("sys");
  sys.path.push("/session/lib/python3.11/site-packages");
  sys.path.push("/session/metadata");
  sys.destroy();
}

export async function loadPyodide() {
  const emscriptenSettings = getEmscriptenSettings();
  const emscriptenModule = await instantiateEmscriptenModule(
    emscriptenSettings
  );
  prepareWasmLinearMemory(emscriptenModule);
  // Finish setting up Pyodide's ffi so we can use the nice Python interface
  emscriptenModule.API.finalizeBootstrap();
  const pyodide = emscriptenModule.API.public_api;
  mountLib(pyodide);
  return pyodide;
}
