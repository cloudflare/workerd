// @ts-nocheck
import { default as ArtifactBundler } from "pyodide-internal:artifacts";
import { default as UnsafeEval } from "internal:unsafe-eval";
import { default as DiskCache } from "pyodide-internal:disk_cache";
import {
  SITE_PACKAGES,
  getSitePackagesPath,
} from "pyodide-internal:setupPackages";
import { default as TarReader } from "pyodide-internal:packages_tar_reader";
import processScriptImports from "pyodide-internal:process_script_imports.py";
import {
  SHOULD_SNAPSHOT_TO_DISK,
  IS_CREATING_BASELINE_SNAPSHOT,
  MEMORY_SNAPSHOT_READER,
} from "pyodide-internal:metadata";
import { reportError, simpleRunPython } from "pyodide-internal:util";

let LOADED_BASELINE_SNAPSHOT;

/**
 * This file is a simplified version of the Pyodide loader:
 * https://github.com/pyodide/pyodide/blob/main/src/js/pyodide.ts
 *
 * In particular, it drops the package lock, which disables
 * `pyodide.loadPackage`. In trade we add memory snapshots here.
 */

const TOP_LEVEL_SNAPSHOT =
  ArtifactBundler.isEwValidating() || SHOULD_SNAPSHOT_TO_DISK;
const SHOULD_UPLOAD_SNAPSHOT =
  ArtifactBundler.isEnabled() || TOP_LEVEL_SNAPSHOT;

/**
 * Global variable for the memory snapshot. On the first run we stick a copy of
 * the linear memory here, on subsequent runs we can skip bootstrapping Python
 * which is quite slow. Startup with snapshot is 3-5 times faster than without
 * it.
 */
let READ_MEMORY = undefined;
let SNAPSHOT_SIZE = undefined;
export let SHOULD_RESTORE_SNAPSHOT = false;

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
function getMemoryToUpload() {
  if (!MEMORY_TO_UPLOAD) {
    throw new TypeError("Expected MEMORY_TO_UPLOAD to be set");
  }
  const tmp = MEMORY_TO_UPLOAD;
  MEMORY_TO_UPLOAD = undefined;
  return tmp;
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

// used for checkLoadedSoFiles a snapshot sanity check
const PRELOADED_SO_FILES = [];

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
export function preloadDynamicLibs(Module) {
  let SO_FILES_TO_LOAD = SITE_PACKAGES.soFiles;
  if (LOADED_BASELINE_SNAPSHOT && LOADED_SNAPSHOT_VERSION === 1) {
    // Ideally this should be just
    // [[ '_lzma.so' ], [ '_ssl.so' ]]
    // but we put a few more because we messed up the memory snapshot...
    SO_FILES_TO_LOAD = [
      ["_hashlib.so"],
      ["_lzma.so"],
      ["_sqlite3.so"],
      ["_ssl.so"],
    ];
  }
  if (
    IS_CREATING_BASELINE_SNAPSHOT ||
    (LOADED_BASELINE_SNAPSHOT && LOADED_SNAPSHOT_VERSION === 2)
  ) {
    SO_FILES_TO_LOAD = [["_lzma.so"], ["_ssl.so"]];
  }
  try {
    const sitePackages = getSitePackagesPath(Module);
    for (const soFile of SO_FILES_TO_LOAD) {
      let node = SITE_PACKAGES.rootInfo;
      for (const part of soFile) {
        node = node.children.get(part);
      }
      const { contentsOffset, size } = node;
      const wasmModuleData = new Uint8Array(size);
      TarReader.read(contentsOffset, wasmModuleData);
      const path = sitePackages + "/" + soFile.join("/");
      PRELOADED_SO_FILES.push(path);
      loadDynlib(Module, path, wasmModuleData);
    }
  } catch (e) {
    console.warn("Error in preloadDynamicLibs");
    reportError(e);
  }
}

export function getSnapshotSettings() {
  return {
    preRun: [preloadDynamicLibs],
    // if SNAPSHOT_SIZE is defined, start with the linear memory big enough to
    // fit the snapshot. If it's not defined, this falls back to the default.
    INITIAL_MEMORY: SNAPSHOT_SIZE,
    // skip running main() if we have a snapshot
    noInitialRun: SHOULD_RESTORE_SNAPSHOT,
  };
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
  dylinkInfo.settings = {};
  if (IS_CREATING_BASELINE_SNAPSHOT) {
    dylinkInfo.settings.baselineSnapshot = true;
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
  if (IS_CREATING_BASELINE_SNAPSHOT) {
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

function checkLoadedSoFiles(dsoJSON) {
  PRELOADED_SO_FILES.sort();
  const keys = Object.keys(dsoJSON).filter((k) => k.startsWith("/"));
  keys.sort();
  const msg = `Internal error taking snapshot: mismatch: ${JSON.stringify(keys)} vs ${JSON.stringify(PRELOADED_SO_FILES)}`;
  if (keys.length !== PRELOADED_SO_FILES.length) {
    throw new Error(msg);
  }
  for (let i = 0; i < keys.length; i++) {
    if (PRELOADED_SO_FILES[i] !== keys[i]) {
      throw new Error(msg);
    }
  }
}

/**
 * Create memory snapshot by importing SNAPSHOT_IMPORTS to ensure these packages
 * are initialized in the linear memory snapshot and then saving a copy of the
 * linear memory into MEMORY.
 */
function makeLinearMemorySnapshot(Module) {
  memorySnapshotDoImports(Module);
  const dsoJSON = recordDsoHandles(Module);
  if (IS_CREATING_BASELINE_SNAPSHOT) {
    // checkLoadedSoFiles(dsoJSON);
  }
  return encodeSnapshot(Module.HEAP8, dsoJSON);
}

function setUploadFunction(toUpload) {
  if (toUpload.constructor.name !== "Uint8Array") {
    throw new TypeError("Expected TO_UPLOAD to be a Uint8Array");
  }
  if (TOP_LEVEL_SNAPSHOT) {
    MEMORY_TO_UPLOAD = toUpload;
    return;
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
      reportError(e);
    }
  };
}

export function maybeSetupSnapshotUpload(Module) {
  if (!SHOULD_UPLOAD_SNAPSHOT) {
    return;
  }
  setUploadFunction(makeLinearMemorySnapshot(Module));
}

// "\x00snp"
const SNAPSHOT_MAGIC = 0x706e7300;
const CREATE_SNAPSHOT_VERSION = 2;
const HEADER_SIZE = 4 * 4;
export let LOADED_SNAPSHOT_VERSION = undefined;

/**
 * Encode heap and dsoJSON into the memory snapshot artifact that we'll upload
 */
function encodeSnapshot(heap, dsoJSON) {
  const dsoString = JSON.stringify(dsoJSON);
  let snapshotOffset = HEADER_SIZE + 2 * dsoString.length;
  // align to 8 bytes
  snapshotOffset = Math.ceil(snapshotOffset / 8) * 8;
  const toUpload = new Uint8Array(snapshotOffset + heap.length);
  const encoder = new TextEncoder();
  const { written: jsonLength } = encoder.encodeInto(
    dsoString,
    toUpload.subarray(HEADER_SIZE),
  );
  const uint32View = new Uint32Array(toUpload.buffer);
  uint32View[0] = SNAPSHOT_MAGIC;
  uint32View[1] = CREATE_SNAPSHOT_VERSION;
  uint32View[2] = snapshotOffset;
  uint32View[3] = jsonLength;
  toUpload.subarray(snapshotOffset).set(heap);
  return toUpload;
}

/**
 * Decode heap and dsoJSON from the memory snapshot artifact we downloaded
 */
function decodeSnapshot() {
  let buf = new Uint32Array(2);
  let offset = 0;
  MEMORY_SNAPSHOT_READER.readMemorySnapshot(offset, buf);
  offset += 8;
  LOADED_SNAPSHOT_VERSION = 0;
  if (buf[0] == SNAPSHOT_MAGIC) {
    LOADED_SNAPSHOT_VERSION = buf[1];
    MEMORY_SNAPSHOT_READER.readMemorySnapshot(offset, buf);
    offset += 8;
  }
  const snapshotOffset = buf[0];
  SNAPSHOT_SIZE =
    MEMORY_SNAPSHOT_READER.getMemorySnapshotSize() - snapshotOffset;
  const jsonLength = buf[1];
  const jsonBuf = new Uint8Array(jsonLength);
  MEMORY_SNAPSHOT_READER.readMemorySnapshot(offset, jsonBuf);
  const jsonTxt = new TextDecoder().decode(jsonBuf);
  DSO_METADATA = JSON.parse(jsonTxt);
  LOADED_BASELINE_SNAPSHOT = Number(DSO_METADATA?.settings?.baselineSnapshot);
  READ_MEMORY = function (Module) {
    // restore memory from snapshot
    MEMORY_SNAPSHOT_READER.readMemorySnapshot(snapshotOffset, Module.HEAP8);
    MEMORY_SNAPSHOT_READER.disposeMemorySnapshot();
  };
  SHOULD_RESTORE_SNAPSHOT = true;
}

export function restoreSnapshot(Module) {
  READ_MEMORY(Module);
}

let TEST_SNAPSHOT = undefined;
(function () {
  try {
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
  } catch (e) {
    console.warn("Error in top level of python.js");
    reportError(e);
  }
})();

export function finishSnapshotSetup(pyodide) {
  if (DSO_METADATA?.settings?.baselineSnapshot) {
    // Invalidate caches if we have a baseline snapshot because the contents of site-packages may
    // have changed.
    simpleRunPython(
      pyodide._module,
      "from importlib import invalidate_caches as f; f(); del f",
    );
  }

  // This is just here for our test suite. Ugly but just about the only way to test this.
  if (TEST_SNAPSHOT) {
    const snapshotString = new TextDecoder().decode(TEST_SNAPSHOT);
    pyodide.registerJsModule("cf_internal_test_utils", {
      snapshot: snapshotString,
    });
  }
}

export function maybeStoreMemorySnapshot() {
  if (ArtifactBundler.isEwValidating()) {
    ArtifactBundler.storeMemorySnapshot(getMemoryToUpload());
  } else if (SHOULD_SNAPSHOT_TO_DISK) {
    DiskCache.put("snapshot.bin", getMemoryToUpload());
    console.log("Saved snapshot to disk");
  }
}
