import { enterJaegerSpan } from 'pyodide-internal:jaeger';
import {
  adjustSysPath,
  mountWorkerFiles,
} from 'pyodide-internal:setupPackages';
import {
  SHOULD_RESTORE_SNAPSHOT,
  finishSnapshotSetup,
  maybeCollectSnapshot,
  restoreSnapshot,
  preloadDynamicLibs,
  shouldCreateSnapshot,
  HIWIRE_STATE,
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
import { reportError } from 'pyodide-internal:util';
import { loadPackages } from 'pyodide-internal:loadPackage';
import { default as MetadataReader } from 'pyodide-internal:runtime-generated/metadata';
import { TRANSITIVE_REQUIREMENTS } from 'pyodide-internal:metadata';

/**
 * After running `instantiateEmscriptenModule` but before calling into any C
 * APIs, we call this function. If `MEMORY` is defined, then we will have passed
 * `noInitialRun: true` and so the C runtime is in an incoherent state until we
 * restore the linear memory from the snapshot.
 */
function prepareWasmLinearMemory(Module: Module): void {
  // Note: if we are restoring from a snapshot, runtime is not initialized yet.
  Module.noInitialRun = !SHOULD_RESTORE_SNAPSHOT;

  enterJaegerSpan('preload_dynamic_libs', () => {
    preloadDynamicLibs(Module);
  });
  enterJaegerSpan('remove_run_dependency', () => {
    Module.removeRunDependency('dynlibs');
  });
  if (SHOULD_RESTORE_SNAPSHOT) {
    enterJaegerSpan('restore_snapshot', () => {
      restoreSnapshot(Module);
    });
    // Invalidate caches if we have a snapshot because the contents of site-packages
    // may have changed.
    simpleRunPython(
      Module,
      'from importlib import invalidate_caches as f; f(); del f'
    );
  }
  // entropyAfterRuntimeInit adjusts JS state ==> always needs to be called.
  entropyAfterRuntimeInit(Module);
  if (!SHOULD_RESTORE_SNAPSHOT) {
    // The effects of these are purely in Python state so they only need to be run
    // if we didn't restore a snapshot.
    entropyBeforeTopLevel(Module);
    adjustSysPath(Module);
  }
  if (Module.API.version !== '0.26.0a2') {
    Module.API.config._makeSnapshot = shouldCreateSnapshot();
    Module.API.finalizeBootstrap(HIWIRE_STATE);
  }
}

function maybeAddVendorDirectoryToPath(pyodide: Pyodide): void {
  pyodide.runPython(`
    def _tmp():
      import sys
      from pathlib import Path

      VENDOR_PATH = "/session/metadata/vendor"
      if Path(VENDOR_PATH).is_dir():
        sys.path.append(VENDOR_PATH)

    _tmp()
    del _tmp
  `);
}

/**
 * Verifies that the Pyodide version in our compat flag matches our actual version. This is to
 * prevent us accidentally releasing a Pyodide bundle built against a different version than one
 * we expect.
 */
function validatePyodideVersion(pyodide: Pyodide): void {
  const expectedPyodideVersion = MetadataReader.getPyodideVersion();
  if (expectedPyodideVersion == 'dev') {
    return;
  }
  if (pyodide.version !== expectedPyodideVersion) {
    throw new Error(
      `Pyodide version mismatch, expected '${expectedPyodideVersion}'`
    );
  }
}

export async function loadPyodide(
  isWorkerd: boolean,
  lockfile: PackageLock,
  indexURL: string
): Promise<Pyodide> {
  try {
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

    entropyMountFiles(Module);
    await enterJaegerSpan('load_packages', () =>
      // NB. loadPackages adds the packages to the `VIRTUALIZED_DIR` global which then gets used in
      // preloadDynamicLibs.
      loadPackages(Module, TRANSITIVE_REQUIREMENTS)
    );

    enterJaegerSpan('prepare_wasm_linear_memory', () => {
      prepareWasmLinearMemory(Module);
    });

    maybeCollectSnapshot(Module);
    // Mount worker files after doing snapshot upload so we ensure that data from the files is never
    // present in snapshot memory.
    mountWorkerFiles(Module);

    if (Module.API.version === '0.26.0a2') {
      // Finish setting up Pyodide's ffi so we can use the nice Python interface
      // In newer versions we already did this in prepareWasmLinearMemory.
      enterJaegerSpan('finalize_bootstrap', Module.API.finalizeBootstrap);
    }
    const pyodide = Module.API.public_api;

    finishSnapshotSetup(pyodide);

    validatePyodideVersion(pyodide);

    // Need to set these here so that the logs go to the right context. If we don't they will go
    // to SetupEmscripten's context and end up being KJ_LOG'd, which we do not want.
    Module.API.initializeStreams(
      null,
      (msg) => {
        console.log(msg);
      },
      (msg) => {
        console.error(msg);
      }
    );
    maybeAddVendorDirectoryToPath(pyodide);
    return pyodide;
  } catch (e) {
    // In edgeworker test suite, without this we get the file name and line number of the exception
    // but no traceback. This gives us a full traceback.
    reportError(e as Error);
  }
}
