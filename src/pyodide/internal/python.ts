import { enterJaegerSpan } from 'pyodide-internal:jaeger';
import {
  VIRTUALIZED_DIR,
  TRANSITIVE_REQUIREMENTS,
  adjustSysPath,
  mountSitePackages,
  mountWorkerFiles,
} from 'pyodide-internal:setupPackages';
import {
  SHOULD_RESTORE_SNAPSHOT,
  finishSnapshotSetup,
  maybeCollectSnapshot,
  restoreSnapshot,
  preloadDynamicLibs,
} from 'pyodide-internal:snapshot';
import {
  entropyMountFiles,
  entropyAfterRuntimeInit,
  entropyBeforeTopLevel,
  getRandomValues,
} from 'pyodide-internal:topLevelEntropy/lib';
/**
 * SetupEmscripten is an internal module defined in setup-emscripten.h the module instantiates
 * emscripten seperately from this code in another context.
 * The underlying code for it can be found in pool/emscriptenSetup.ts.
 */
import { default as SetupEmscripten } from 'internal:setup-emscripten';

import { default as UnsafeEval } from 'internal:unsafe-eval';
import { simpleRunPython } from 'pyodide-internal:util';
import { loadPackages } from 'pyodide-internal:loadPackage';

/**
 * After running `instantiateEmscriptenModule` but before calling into any C
 * APIs, we call this function. If `MEMORY` is defined, then we will have passed
 * `noInitialRun: true` and so the C runtime is in an incoherent state until we
 * restore the linear memory from the snapshot.
 */
function prepareWasmLinearMemory(Module: Module): void {
  // Note: if we are restoring from a snapshot, runtime is not initialized yet.
  Module.noInitialRun = !SHOULD_RESTORE_SNAPSHOT;

  enterJaegerSpan('preload_dynamic_libs', () => preloadDynamicLibs(Module));
  enterJaegerSpan('remove_run_dependency', () =>
    Module.removeRunDependency('dynlibs')
  );
  if (SHOULD_RESTORE_SNAPSHOT) {
    enterJaegerSpan('restore_snapshot', () => restoreSnapshot(Module));
    // Invalidate caches if we have a snapshot because the contents of site-packages
    // may have changed.
    simpleRunPython(
      Module,
      'from importlib import invalidate_caches as f; f(); del f'
    );
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
  isWorkerd: boolean,
  lockfile: PackageLock,
  indexURL: string
): Promise<Pyodide> {
  const Module = enterJaegerSpan('instantiate_emscripten', () =>
    SetupEmscripten.getModule()
  );
  Module.API.config.jsglobals = globalThis;
  if (isWorkerd) {
    Module.API.config.indexURL = indexURL;
    Module.API.config.resolveLockFilePromise!(lockfile);
  }
  Module.setUnsafeEval(UnsafeEval);
  Module.setGetRandomValues(getRandomValues);

  mountSitePackages(Module, VIRTUALIZED_DIR);
  entropyMountFiles(Module);
  await enterJaegerSpan('load_packages', () =>
    // NB. loadPackages adds the packages to the `VIRTUALIZED_DIR` global which then gets used in
    // preloadDynamicLibs.
    loadPackages(Module, TRANSITIVE_REQUIREMENTS)
  );

  enterJaegerSpan('prepare_wasm_linear_memory', () =>
    prepareWasmLinearMemory(Module)
  );

  maybeCollectSnapshot(Module);
  // Mount worker files after doing snapshot upload so we ensure that data from the files is never
  // present in snapshot memory.
  mountWorkerFiles(Module);

  // Finish setting up Pyodide's ffi so we can use the nice Python interface
  enterJaegerSpan('finalize_bootstrap', Module.API.finalizeBootstrap);
  const pyodide = Module.API.public_api;

  finishSnapshotSetup(pyodide);
  return pyodide;
}
