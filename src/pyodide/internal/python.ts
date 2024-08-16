Error.stackTraceLimit = Infinity;
import { enterJaegerSpan } from 'pyodide-internal:jaeger';
import {
  SITE_PACKAGES,
  adjustSysPath,
  mountLib,
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
} from 'pyodide-internal:topLevelEntropy/lib';
import { setupEmscriptenModule } from 'pyodide-internal:emscriptenSetup';

/**
 * After running `instantiateEmscriptenModule` but before calling into any C
 * APIs, we call this function. If `MEMORY` is defined, then we will have passed
 * `noInitialRun: true` and so the C runtime is in an incoherent state until we
 * restore the linear memory from the snapshot.
 */
async function prepareWasmLinearMemory(Module: Module): Promise<void> {
  // Note: if we are restoring from a snapshot, runtime is not initialized yet.
  mountLib(Module, SITE_PACKAGES.rootInfo);
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
  const Module = await setupEmscriptenModule(lockfile, indexURL);
  // Finish setting up Pyodide's ffi so we can use the nice Python interface
  await enterJaegerSpan('prepare_wasm_linear_memory', () =>
    prepareWasmLinearMemory(Module)
  );
  maybeSetupSnapshotUpload(Module);
  await enterJaegerSpan('finalize_bootstrap', Module.API.finalizeBootstrap);
  const pyodide = Module.API.public_api;
  finishSnapshotSetup(pyodide);
  return pyodide;
}
