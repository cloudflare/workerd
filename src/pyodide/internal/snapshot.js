import { default as ArtifactBundler } from "pyodide-internal:artifacts";
import { default as UnsafeEval } from "internal:unsafe-eval";
import {
  SITE_PACKAGES_INFO,
  SITE_PACKAGES_SO_FILES,
  getSitePackagesPath,
} from "pyodide-internal:setupPackages";
import { default as TarReader } from "pyodide-internal:packages_tar_reader";
import processScriptImports from "pyodide-internal:process_script_imports.py";
import { simpleRunPython } from "pyodide-internal:simpleRunPython";

import { MEMORY_SNAPSHOT_READER } from "pyodide-internal:metadata";

export const SHOULD_UPLOAD_SNAPSHOT =
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
  return MEMORY_TO_UPLOAD;
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
  SNAPSHOT_SIZE =
    MEMORY_SNAPSHOT_READER.getMemorySnapshotSize() - snapshotOffset;
  const jsonLength = buf[1];
  const jsonBuf = new Uint8Array(jsonLength);
  MEMORY_SNAPSHOT_READER.readMemorySnapshot(8, jsonBuf);
  DSO_METADATA = JSON.parse(new TextDecoder().decode(jsonBuf));
  READ_MEMORY = function (Module) {
    // restore memory from snapshot
    MEMORY_SNAPSHOT_READER.readMemorySnapshot(snapshotOffset, Module.HEAP8);
    MEMORY_SNAPSHOT_READER.disposeMemorySnapshot();
  };
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

/*
 * This is just here for our test suite. Ugly but just about the only way to test this.
 */
export function maybeSetUpSnapshotTest(pyodide) {
  if (!TEST_SNAPSHOT) {
    return;
  }
  const snapshotString = new TextDecoder().decode(TEST_SNAPSHOT);
  pyodide.registerJsModule("cf_internal_test_utils", {
    snapshot: snapshotString,
  });
}

// Snapshot-related settings to merge into the main emscripten settings object
// in python.js
export const SNAPSHOT_EMSCRIPTEN_SETTINGS = {
  // if SNAPSHOT_SIZE is defined, start with the linear memory big enough to
  // fit the snapshot. If it's not defined, this falls back to the default.
  INITIAL_MEMORY: SNAPSHOT_SIZE,
  // skip running main() if we have a snapshot
  noInitialRun: !!READ_MEMORY,
  preRun: [preloadDynamicLibs],
};

/**
 * If there is a memory snapshot ready, restore it and return true. Otherwise,
 * return false.
 */
export function maybeRestoreSnapshot(Module) {
  if (!READ_MEMORY) {
    return false;
  }
  READ_MEMORY(Module);
  // Might as well clean up here, we don't need it again
  READ_MEMORY = undefined;
  return true;
}

/**
 * If we've been asked to upload a memory snapshot, prepare to upload it. This
 *  does whatever imports we want to include in the memory snapshot and then
 *  stores it in the proper spot, we don't actually use it until a bit later.
 */
export function maybePrepareSnapshotForUpload(Module) {
  if (!SHOULD_UPLOAD_SNAPSHOT) {
    return;
  }
  setUploadFunction(makeLinearMemorySnapshot(Module));
}
