import { enterJaegerSpan } from 'pyodide-internal:jaeger';
import {
  adjustSysPath,
  mountWorkerFiles,
} from 'pyodide-internal:setupPackages';
import {
  maybeCollectSnapshot,
  maybeRestoreSnapshot,
  finalizeBootstrap,
  isRestoringSnapshot,
} from 'pyodide-internal:snapshot';
import {
  entropyMountFiles,
  entropyAfterRuntimeInit,
  entropyBeforeTopLevel,
  getRandomValues,
  entropyBeforeRequest,
} from 'pyodide-internal:topLevelEntropy/lib';
import { LEGACY_VENDOR_PATH } from 'pyodide-internal:metadata';
import type { PyodideEntrypointHelper } from 'pyodide:python-entrypoint-helper';

/**
 * SetupEmscripten is an internal module defined in setup-emscripten.h the module instantiates
 * emscripten seperately from this code in another context.
 * The underlying code for it can be found in pool/emscriptenSetup.ts.
 */
import { default as SetupEmscripten } from 'internal:setup-emscripten';

import { default as UnsafeEval } from 'internal:unsafe-eval';
import { PythonRuntimeError, reportError } from 'pyodide-internal:util';
import { loadPackages } from 'pyodide-internal:loadPackage';
import { default as MetadataReader } from 'pyodide-internal:runtime-generated/metadata';
import { TRANSITIVE_REQUIREMENTS } from 'pyodide-internal:metadata';

/**
 * After running `instantiateEmscriptenModule` but before calling into any C
 * APIs, we call this function. If `MEMORY` is defined, then we will have passed
 * `noInitialRun: true` and so the C runtime is in an incoherent state until we
 * restore the linear memory from the snapshot.
 */
function prepareWasmLinearMemory(
  Module: Module,
  pyodide_entrypoint_helper: PyodideEntrypointHelper
): void {
  maybeRestoreSnapshot(Module);
  // entropyAfterRuntimeInit adjusts JS state ==> always needs to be called.
  entropyAfterRuntimeInit(Module);
  if (!isRestoringSnapshot()) {
    // The effects of these are purely in Python state so they only need to be run
    // if we didn't restore a snapshot.
    entropyBeforeTopLevel(Module);
    // Note that setupPythonSearchPath runs after adjustSysPath and rearranges where
    // the /session/metadata path is added.
    adjustSysPath(Module);
  }
  if (Module.API.version !== '0.26.0a2') {
    finalizeBootstrap(Module, pyodide_entrypoint_helper);
  }
}

function setupPythonSearchPath(pyodide: Pyodide): void {
  pyodide.runPython(`
    def _tmp():
      import sys
      from pathlib import Path

      LEGACY_VENDOR_PATH = "${LEGACY_VENDOR_PATH}" == "true"
      VENDOR_PATH = "/session/metadata/vendor"
      PYTHON_MODULES_PATH = "/session/metadata/python_modules"

      # adjustSysPath adds the session path, but it is immortalised by the memory snapshot. This
      # code runs irrespective of the memory snapshot.
      if VENDOR_PATH in sys.path and LEGACY_VENDOR_PATH:
        sys.path.remove(VENDOR_PATH)

      if PYTHON_MODULES_PATH in sys.path:
        sys.path.remove(PYTHON_MODULES_PATH)

      # Insert vendor path after system paths but before site-packages
      # System paths are typically: ['/session', '/lib/python312.zip', '/lib/python3.12', '/lib/python3.12/lib-dynload']
      # We want to insert before '/lib/python3.12/site-packages' and other site-packages
      #
      # We also need the session path to be before the vendor path, if we don't do so then a local
      # import will pick a module from the vendor path rather than the local path. We've got a test
      # that reproduces this (vendor_dir).
      for i, path in enumerate(sys.path):
        if 'site-packages' in path:
          if LEGACY_VENDOR_PATH:
            sys.path.insert(i, VENDOR_PATH)
          sys.path.insert(i, PYTHON_MODULES_PATH)
          break
      else:
        # If no site-packages found, fail
        raise ValueError("No site-packages found in sys.path")

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
    throw new PythonRuntimeError(
      `Pyodide version mismatch, expected '${expectedPyodideVersion}'`
    );
  }
}

const origSetTimeout = globalThis.setTimeout.bind(this);
function setTimeoutTopLevelPatch(
  handler: () => void,
  timeout: number | undefined
): number {
  // Redirect top level setTimeout(cb, 0) to queueMicrotask().
  // If we don't know how to handle it, call normal setTimeout() to force failure.
  if (typeof handler === 'string') {
    return origSetTimeout(handler, timeout);
  }
  if (timeout) {
    return origSetTimeout(handler, timeout);
  }
  queueMicrotask(handler);
  return 0;
}

export function loadPyodide(
  isWorkerd: boolean,
  lockfile: PackageLock,
  indexURL: string,
  pyodide_entrypoint_helper: PyodideEntrypointHelper
): Pyodide {
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
    Module.setSetTimeout(
      setTimeoutTopLevelPatch as typeof setTimeout,
      clearTimeout,
      setInterval,
      clearInterval
    );

    entropyMountFiles(Module);
    enterJaegerSpan('load_packages', () => {
      // NB. loadPackages adds the packages to the `VIRTUALIZED_DIR` global which then gets used in
      // preloadDynamicLibs.
      loadPackages(Module, TRANSITIVE_REQUIREMENTS);
    });

    enterJaegerSpan('prepare_wasm_linear_memory', () => {
      prepareWasmLinearMemory(Module, pyodide_entrypoint_helper);
    });

    maybeCollectSnapshot(Module);
    // Mount worker files after doing snapshot upload so we ensure that data from the files is never
    // present in snapshot memory.
    mountWorkerFiles(Module);

    if (Module.API.version === '0.26.0a2') {
      // Finish setting up Pyodide's ffi so we can use the nice Python interface
      // In newer versions we already did this in prepareWasmLinearMemory.
      finalizeBootstrap(Module, pyodide_entrypoint_helper);
    }
    const pyodide = Module.API.public_api;

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
    setupPythonSearchPath(pyodide);
    return pyodide;
  } catch (e) {
    // In edgeworker test suite, without this we get the file name and line number of the exception
    // but no traceback. This gives us a full traceback.
    reportError(e as Error);
  }
}

export function beforeRequest(Module: Module): void {
  entropyBeforeRequest(Module);
  Module.setSetTimeout(setTimeout, clearTimeout, setInterval, clearInterval);
}
