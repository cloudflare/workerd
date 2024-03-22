import { enterJaegerSpan } from "pyodide-internal:jaeger";
import {
  SITE_PACKAGES_INFO,
  adjustSysPath,
  mountLib,
} from "pyodide-internal:setupPackages";
import {
  maybePrepareSnapshotForUpload,
  maybeRestoreSnapshot,
  maybeSetUpSnapshotTest,
  SNAPSHOT_EMSCRIPTEN_SETTINGS,
} from "pyodide-internal:snapshot";

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
      wasmImports,
    );
    successCallback(instance, pyodideWasmModule);
  })();

  return {};
}

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
 * directory `/lib/pythonv.vv/site-packages`.
 *
 * This is a simpified version of the `prepareFileSystem` function here:
 * https://github.com/pyodide/pyodide/blob/main/src/js/module.ts
 */
function prepareFileSystem(Module) {
  try {
    const pymajor = Module._py_version_major();
    const pyminor = Module._py_version_minor();
    Module.FS.mkdirTree(`/lib/python${pymajor}.${pyminor}/site-packages`);
    Module.FS.writeFile(
      `/lib/python${pymajor}${pyminor}.zip`,
      new Uint8Array(stdlib),
      { canOwn: true },
    );
    Module.FS.mkdirTree(Module.API.config.env.HOME);
  } catch (e) {
    console.warn(e);
  }
}

/**
 * A preRun hook. Make sure environment variables are visible at runtime.
 */
function setEnv(Module) {
  Object.assign(Module.ENV, Module.API.config.env);
}

/**
 * The Emscripten settings object
 *
 * This isn't public API of Pyodide so it's a bit fiddly.
 */
function getEmscriptenSettings(lockfile, indexURL) {
  const config = {
    // jsglobals is used for the js module.
    jsglobals: globalThis,
    // environment variables go here
    env: {
      HOME: "/session",
      // We don't have access to cryptographic rng at startup so we cannot support hash
      // randomization. Setting `PYTHONHASHSEED` disables it.
      PYTHONHASHSEED: "111",
    },
    // This is the index that we use as the base URL to fetch the wheels.
    indexURL,
  };
  // loadPackage initializes its state using lockFilePromise.
  const lockFilePromise = lockfile ? Promise.resolve(lockfile) : undefined;
  const API = { config, lockFilePromise };
  // Emscripten settings to control runtime instantiation.
  const { preRun: snapshotPrerun, ...snapshotEmscriptenSettings } =
    SNAPSHOT_EMSCRIPTEN_SETTINGS;
  return {
    // preRun hook to set up the file system before running main
    // The preRun hook gets run independently of noInitialRun, which is
    // important because the file system lives outside of linear memory.
    preRun: [prepareFileSystem, setEnv, ...snapshotPrerun],
    instantiateWasm,
    API, // Pyodide requires we pass this in.
    ...snapshotEmscriptenSettings,
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
    // If/when we link our own Pyodide we can remove this.
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
 *
 * Returns `true` when existing memory snapshot was loaded.
 */
async function prepareWasmLinearMemory(Module) {
  // Note: if we are restoring from a snapshot, runtime is not initialized yet.
  mountLib(Module, SITE_PACKAGES_INFO);
  if (maybeRestoreSnapshot(Module)) {
    // We don't call adjustSysPath here: it was called in the other branch when
    // we were creating the snapshot so the outcome of that is already baked in.
    return;
  }
  adjustSysPath(Module);
  // If we are validating or in
  maybePrepareSnapshotForUpload(Module);
}

export async function loadPyodide(lockfile, indexURL) {
  const emscriptenSettings = getEmscriptenSettings(lockfile, indexURL);
  const Module = await enterJaegerSpan("instantiate_emscripten", () =>
    instantiateEmscriptenModule(emscriptenSettings),
  );
  await enterJaegerSpan("prepare_wasm_linear_memory", () =>
    prepareWasmLinearMemory(Module),
  );

  // Finish setting up Pyodide's ffi so we can use the nice Python interface
  await enterJaegerSpan("finalize_bootstrap", Module.API.finalizeBootstrap);
  const pyodide = Module.API.public_api;
  maybeSetUpSnapshotTest(pyodide);
  return pyodide;
}
