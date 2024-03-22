import { default as ArtifactBundler } from "pyodide-internal:artifacts";
import { enterJaegerSpan } from "pyodide-internal:jaeger";
import { default as UnsafeEval } from "internal:unsafe-eval";
import {
  SITE_PACKAGES_INFO,
  SITE_PACKAGES_SO_FILES,
  adjustSysPath,
  getSitePackagesPath,
  mountLib,
} from "pyodide-internal:setupPackages";
import { default as TarReader } from "pyodide-internal:packages_tar_reader";
import processScriptImports from "pyodide-internal:process_script_imports.py";
import { MEMORY_SNAPSHOT_READER } from "pyodide-internal:metadata";

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

const SHOULD_UPLOAD_SNAPSHOT =
  ArtifactBundler.isEnabled() || ArtifactBundler.isEwValidating();
const DEDICATED_SNAPSHOT = true;

/**
 * Global variable for the memory snapshot. On the first run we stick a copy of
 * the linear memory here, on subsequent runs we can skip bootstrapping Python
 * which is quite slow. Startup with snapshot is 3-5 times faster than without
 * it.
 */
let READ_MEMORY = undefined;
let SNAPSHOT_SIZE = undefined;

/**
 * Record the dlopen handles that are needed by the MEMORY.
 */
let DSO_METADATA = {};

/**
 * Used to defer artifact upload. This is set during initialisation, but is executed during a
 * request because an IO context is needed for the upload.
 */
let DEFERRED_UPLOAD_FUNCTION = undefined;

export async function uploadArtifacts() {
  if (DEFERRED_UPLOAD_FUNCTION) {
    return await DEFERRED_UPLOAD_FUNCTION();
  }
}

/**
 * Used to hold the memory that needs to be uploaded for the validator.
 */
let MEMORY_TO_UPLOAD = undefined;
export function getMemoryToUpload() {
  if (!MEMORY_TO_UPLOAD) {
    throw new TypeError("Expected MEMORY_TO_UPLOAD to be set");
  }
  const tmp = MEMORY_TO_UPLOAD;
  MEMORY_TO_UPLOAD = undefined;
  return tmp;
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
 * Preload a dynamic library.
 *
 * Emscripten would usually figure out all of these details for us
 * automatically. These defaults work for shared libs that are configured as
 * standard Python extensions. This naive approach will not work for libraries
 * like scipy, shapely, geos...
 * TODO(someday) fix this.
 */
function loadDynlib(Module, path, wasmModuleData) {
  const wasmModule = UnsafeEval.newWasmModule(wasmModuleData);
  const dso = Module.newDSO(path, undefined, "loading");
  // even though these are used via dlopen, we are allocating them in an arena
  // outside the heap and the memory cannot be reclaimed. So I don't think it
  // would help us to allow them to be dealloc'd.
  dso.refcount = Infinity;
  // Hopefully they are used with dlopen
  dso.global = false;
  dso.exports = Module.loadWebAssemblyModule(wasmModule, {}, path);
  // "handles" are dlopen handles. There will be one entry in the `handles` list
  // for each dlopen handle that has not been dlclosed. We need to keep track of
  // these across
  const { handles } = DSO_METADATA[path] || { handles: [] };
  for (const handle of handles) {
    Module.LDSO.loadedLibsByHandle[handle] = dso;
  }
}

/**
 * This loads all dynamic libraries visible in the site-packages directory. They
 * are loaded before the runtime is initialized outside of the heap, using the
 * same mechanism for DT_NEEDED libs (i.e., the libs that are loaded before the
 * program starts because you passed them as linker args).
 *
 * Currently, we pessimistically preload all libs. It would be nice to only load
 * the ones that are used. I am pretty sure we can manage this by reserving a
 * separate shared lib metadata arena at startup and allocating shared libs
 * there.
 */
function preloadDynamicLibs(Module) {
  try {
    const sitePackages = getSitePackagesPath(Module);
    for (const soFile of SITE_PACKAGES_SO_FILES) {
      let node = SITE_PACKAGES_INFO;
      for (const part of soFile) {
        node = node.children.get(part);
      }
      const { contentsOffset, size } = node;
      const wasmModuleData = new Uint8Array(size);
      TarReader.read(contentsOffset, wasmModuleData);
      const path = sitePackages + "/" + soFile.join("/");
      loadDynlib(Module, path, wasmModuleData);
    }
  } catch (e) {
    console.log(e);
    throw e;
  }
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
  return {
    // preRun hook to set up the file system before running main
    // The preRun hook gets run independently of noInitialRun, which is
    // important because the file system lives outside of linear memory.
    preRun: [prepareFileSystem, setEnv, preloadDynamicLibs],
    instantiateWasm,
    // if SNAPSHOT_SIZE is defined, start with the linear memory big enough to
    // fit the snapshot. If it's not defined, this falls back to the default.
    INITIAL_MEMORY: SNAPSHOT_SIZE,
    noInitialRun: !!READ_MEMORY, // skip running main() if we have a snapshot
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
  if (READ_MEMORY) {
    READ_MEMORY(Module);
    // Don't call adjustSysPath here: it was called in the other branch when we
    // were creating the snapshot so the outcome of that is already baked in.
    return;
  }
  adjustSysPath(Module, simpleRunPython);

  if (SHOULD_UPLOAD_SNAPSHOT) {
    setUploadFunction(makeLinearMemorySnapshot(Module));
  }
}

/**
 * This records which dynamic libraries have open handles (handed out by dlopen,
 * not yet dlclosed). We'll need to track this information so that we don't
 * crash if we dlsym the handle after restoring from the snapshot
 */
function recordDsoHandles(Module) {
  const dylinkInfo = {};
  for (const [handle, { name }] of Object.entries(
    Module.LDSO.loadedLibsByHandle,
  )) {
    if (handle === 0) {
      continue;
    }
    if (!(name in dylinkInfo)) {
      dylinkInfo[name] = { handles: [] };
    }
    dylinkInfo[name].handles.push(handle);
  }
  return dylinkInfo;
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
  // Asyncio is the really slow one here. In native Python on my machine, `import asyncio` takes ~50
  // ms.
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
 * Python modules do a lot of work the first time they are imported. The memory
 * snapshot will save more time the more of this work is included. However, we
 * can't snapshot the JS runtime state so we have no ffi. Thus some imports from
 * user code will fail.
 *
 * If we are doing a baseline snapshot, just import everything from
 * SNAPSHOT_IMPORTS. These will all succeed.
 *
 * If doing a script-specific "dedicated" snap shot, also try to import each
 * user import.
 *
 * All of this is being done in the __main__ global scope, so be careful not to
 * pollute it with extra included-by-default names (user code is executed in its
 * own separate module scope though so it's not _that_ important).
 */
function memorySnapshotDoImports(Module) {
  const toImport = SNAPSHOT_IMPORTS.join(",");
  const toDelete = Array.from(
    new Set(SNAPSHOT_IMPORTS.map((x) => x.split(".", 1)[0])),
  ).join(",");
  simpleRunPython(Module, `import ${toImport}`);
  simpleRunPython(Module, "sysconfig.get_config_vars()");
  // Delete to avoid polluting globals
  simpleRunPython(Module, `del ${toDelete}`);
  if (!DEDICATED_SNAPSHOT) {
    // We've done all the imports for the baseline snapshot.
    return;
  }
  // Script-specific imports: collect all import nodes from user scripts and try
  // to import them, catching and throwing away all failures.
  // see process_script_imports.py.
  const processScriptImportsString = new TextDecoder().decode(
    new Uint8Array(processScriptImports),
  );
  simpleRunPython(Module, processScriptImportsString);
}

/**
 * Create memory snapshot by importing SNAPSHOT_IMPORTS to ensure these packages
 * are initialized in the linear memory snapshot and then saving a copy of the
 * linear memory into MEMORY.
 */
function makeLinearMemorySnapshot(Module) {
  memorySnapshotDoImports(Module);
  const dsoJSON = recordDsoHandles(Module);
  return encodeSnapshot(Module.HEAP8, dsoJSON);
}

function setUploadFunction(toUpload) {
  if (toUpload.constructor.name !== "Uint8Array") {
    throw new TypeError("Expected TO_UPLOAD to be a Uint8Array");
  }
  if (ArtifactBundler.isEwValidating()) {
    MEMORY_TO_UPLOAD = toUpload;
  }
  DEFERRED_UPLOAD_FUNCTION = async () => {
    try {
      const success = await ArtifactBundler.uploadMemorySnapshot(toUpload);
      // Free memory
      toUpload = undefined;
      if (!success) {
        console.warn("Memory snapshot upload failed.");
      }
    } catch (e) {
      console.warn("Memory snapshot upload failed.");
      console.warn(e);
      throw e;
    }
  };
}

/**
 * Encode heap and dsoJSON into the memory snapshot artifact that we'll upload
 */
function encodeSnapshot(heap, dsoJSON) {
  const dsoString = JSON.stringify(dsoJSON);
  let sx = 8 + 2 * dsoString.length;
  // align to 8 bytes
  sx = Math.ceil(sx / 8) * 8;
  const toUpload = new Uint8Array(sx + heap.length);
  const encoder = new TextEncoder();
  const { written } = encoder.encodeInto(dsoString, toUpload.subarray(8));
  const uint32View = new Uint32Array(toUpload.buffer);
  uint32View[0] = sx;
  uint32View[1] = written;
  toUpload.subarray(sx).set(heap);
  return toUpload;
}

/**
 * Decode heap and dsoJSON from the memory snapshot artifact we downloaded
 */
function decodeSnapshot() {
  const buf = new Uint32Array(2);
  MEMORY_SNAPSHOT_READER.readMemorySnapshot(0, buf);
  const snapshotOffset = buf[0];
  SNAPSHOT_SIZE = MEMORY_SNAPSHOT_READER.getMemorySnapshotSize() - snapshotOffset;
  const jsonLength = buf[1];
  const jsonBuf = new Uint8Array(jsonLength);
  MEMORY_SNAPSHOT_READER.readMemorySnapshot(8, jsonBuf);
  DSO_METADATA = JSON.parse(new TextDecoder().decode(jsonBuf));
  READ_MEMORY = function(Module) {
    // restore memory from snapshot
    MEMORY_SNAPSHOT_READER.readMemorySnapshot(snapshotOffset, Module.HEAP8);
    MEMORY_SNAPSHOT_READER.disposeMemorySnapshot();
  }
}

/**
 *  Simple as possible runPython function which works with no foreign function
 *  interface. We need to use this rather than the normal easier to use
 *  interface because the normal interface doesn't work until after
 *  `API.finalizeBootstrap`, but `API.finalizeBootstrap` makes changes inside
 *  and outside the linear memory which have to stay in sync. It's hard to keep
 *  track of the invariants that `finalizeBootstrap` introduces between JS land
 *  and the linear memory so we do this.
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

let TEST_SNAPSHOT = undefined;
(function () {
  // Lookup memory snapshot from artifact store.
  if (!MEMORY_SNAPSHOT_READER) {
    // snapshots are disabled or there isn't one yet
    return;
  }

  // Simple sanity check to ensure this snapshot isn't corrupted.
  //
  // TODO(later): we need better detection when this is corrupted. Right now the isolate will
  // just die.
  const snapshotSize = MEMORY_SNAPSHOT_READER.getMemorySnapshotSize();
  if (snapshotSize <= 100) {
    TEST_SNAPSHOT = new Uint8Array(snapshotSize);
    MEMORY_SNAPSHOT_READER.readMemorySnapshot(0, TEST_SNAPSHOT);
    return;
  }
  decodeSnapshot();
})();

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

  // This is just here for our test suite. Ugly but just about the only way to test this.
  if (TEST_SNAPSHOT) {
    const snapshotString = new TextDecoder().decode(TEST_SNAPSHOT);
    pyodide.registerJsModule("cf_internal_test_utils", {
      snapshot: snapshotString,
    });
  }
  return pyodide;
}
