import { enterJaegerSpan } from 'pyodide-internal:jaeger';
import {
  SITE_PACKAGES,
  adjustSysPath,
  mountSitePackages,
  mountWorkerFiles,
} from 'pyodide-internal:setupPackages';
import {
  SHOULD_RESTORE_SNAPSHOT,
  finishSnapshotSetup,
  maybeSetupSnapshotUpload,
  restoreSnapshot,
  preloadDynamicLibs,
} from 'pyodide-internal:snapshot';
import {
  entropyMountFiles,
  entropyAfterRuntimeInit,
  entropyBeforeTopLevel,
  getRandomValues,
} from 'pyodide-internal:topLevelEntropy/lib';
import { default as UnsafeEval } from 'internal:unsafe-eval';

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
import pyodideWasmModule from 'pyodide-internal:generated/pyodide.asm.wasm';

/**
 * The Python and Pyodide stdlib zipped together. The zip format is convenient
 * because Python has a "ziploader" that allows one to import directly from a
 * zip file.
 *
 * The ziploader solves bootstrapping problems around unpacking: Python comes
 * with a bunch of C libs to unpack various archive formats, but they need stuff
 * in this zip file to initialize their runtime state.
 */
import pythonStdlib from 'pyodide-internal:generated/python_stdlib.zip';
import {
  instantiateEmscriptenModule,
  setUnsafeEval,
  setGetRandomValues,
} from 'pyodide-internal:generated/emscriptenSetup';

/**
 * After running `instantiateEmscriptenModule` but before calling into any C
 * APIs, we call this function. If `MEMORY` is defined, then we will have passed
 * `noInitialRun: true` and so the C runtime is in an incoherent state until we
 * restore the linear memory from the snapshot.
 */
async function prepareWasmLinearMemory(Module: Module): Promise<void> {
  // Note: if we are restoring from a snapshot, runtime is not initialized yet.
  mountSitePackages(Module, SITE_PACKAGES.rootInfo);
  entropyMountFiles(Module);
  Module.noInitialRun = !SHOULD_RESTORE_SNAPSHOT;
  preloadDynamicLibs(Module);
  Module.removeRunDependency('dynlibs');
  if (SHOULD_RESTORE_SNAPSHOT) {
    restoreSnapshot(Module);
  }
  // entropyAfterRuntimeInit adjusts JS state ==> always needs to be called.
  entropyAfterRuntimeInit(Module);
  if (SHOULD_RESTORE_SNAPSHOT) {
    return;
  }
  // The effects of these are purely in Python state so they only need to be run
  // if we didn't restore a snapshot.
  entropyBeforeTopLevel(Module);
  adjustSysPath(Module);
}

export async function loadPyodide(
  lockfile: PackageLock,
  indexURL: string
): Promise<Pyodide> {
  const Module = await enterJaegerSpan('instantiate_emscripten', () =>
    instantiateEmscriptenModule(
      lockfile,
      indexURL,
      pythonStdlib,
      pyodideWasmModule
    )
  );
  setUnsafeEval(UnsafeEval);
  setGetRandomValues(getRandomValues);
  await enterJaegerSpan('prepare_wasm_linear_memory', () =>
    prepareWasmLinearMemory(Module)
  );
  maybeSetupSnapshotUpload(Module);
  // Mount worker files after doing snapshot upload so we ensure that data from the files is never
  // present in snapshot memory.
  mountWorkerFiles(Module);

  // Finish setting up Pyodide's ffi so we can use the nice Python interface
  await enterJaegerSpan('finalize_bootstrap', Module.API.finalizeBootstrap);
  const pyodide = Module.API.public_api;
  finishSnapshotSetup(pyodide);
  return pyodide;
}
