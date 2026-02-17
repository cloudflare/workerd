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
  type CustomSerializedObjects,
} from 'pyodide-internal:snapshot';
import {
  entropyMountFiles,
  entropyAfterRuntimeInit,
  entropyBeforeTopLevel,
  getRandomValues,
  entropyBeforeRequest,
} from 'pyodide-internal:topLevelEntropy/lib';
import {
  LEGACY_VENDOR_PATH,
  setCpuLimitNearlyExceededCallback,
} from 'pyodide-internal:metadata';

/**
 * SetupEmscripten is an internal module defined in setup-emscripten.h the module instantiates
 * emscripten seperately from this code in another context.
 * The underlying code for it can be found in pool/emscriptenSetup.ts.
 */
import { default as SetupEmscripten } from 'internal:setup-emscripten';

import { default as UnsafeEval } from 'internal:unsafe-eval';
import {
  PythonUserError,
  PythonWorkersInternalError,
  reportError,
  unreachable,
} from 'pyodide-internal:util';
import { loadPackages } from 'pyodide-internal:loadPackage';
import { default as MetadataReader } from 'pyodide-internal:runtime-generated/metadata';
import { TRANSITIVE_REQUIREMENTS } from 'pyodide-internal:metadata';
import { getTrustedReadFunc } from 'pyodide-internal:readOnlyFS';

/**
 * After running `instantiateEmscriptenModule` but before calling into any C
 * APIs, we call this function. If `MEMORY` is defined, then we will have passed
 * `noInitialRun: true` and so the C runtime is in an incoherent state until we
 * restore the linear memory from the snapshot.
 */
function prepareWasmLinearMemory(
  Module: Module,
  customSerializedObjects: CustomSerializedObjects
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
    finalizeBootstrap(Module, customSerializedObjects);
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
    throw new PythonWorkersInternalError(
      `Pyodide version mismatch, expected '${expectedPyodideVersion}'`
    );
  }
}

const origSetTimeout = globalThis.setTimeout.bind(this);

function makeSetTimeout(Module: Module): typeof setTimeout {
  return function setTimeoutTopLevelPatch(
    handler: () => void,
    timeout: number | undefined
  ): number {
    // Redirect top level setTimeout(cb, 0) to queueMicrotask().
    // If we don't know how to handle it, call normal setTimeout() to force failure.
    if (typeof handler === 'string') {
      return origSetTimeout(handler, timeout);
    }
    function wrappedHandler(): void {
      // In case an Exceeded CPU occurred just as Python was exiting, there may be one waiting that
      // will interrupt the wrong task. Clear signals before entering the task.
      // This is covered by cpu-limit-exceeded.ew-test "async_trip" test.
      clearSignals(Module);
      handler();
    }
    if (timeout) {
      return origSetTimeout(wrappedHandler, timeout);
    }
    queueMicrotask(wrappedHandler);
    return 0;
  } as typeof setTimeout;
}

function getSignalClockAddr(Module: Module): number {
  if (Module.API.version !== '0.28.2') {
    throw new PythonWorkersInternalError(
      'getSignalClockAddr only supported in 0.28.2'
    );
  }
  // This is the address here:
  // https://github.com/python/cpython/blob/main/Python/emscripten_signal.c#L42
  //
  // Since the symbol isn't exported, we can't access it directly. Instead, we used wasm-objdump and
  // searched for the call site to _Py_CheckEmscriptenSignals_Helper(), then read the offset out of
  // the assembly code.
  //
  // TODO: Export this symbol in the next Pyodide release so we can stop using the magic number.
  const emscripten_signal_clock_offset = 3171536;
  return Module.___memory_base.value + emscripten_signal_clock_offset;
}

function setupRuntimeSignalHandling(Module: Module): void {
  Module.Py_EmscriptenSignalBuffer = new Uint8Array(1);
  const version = Module.API.version;
  if (version === '0.26.0a2') {
    return;
  }
  if (version === '0.28.2') {
    // The callback sets signal_clock to 0 and signal_handling to 1. It has to be in C++ because we
    // don't hold the isolate lock when we call it. JS code would be:
    //
    // function callback() { Module.HEAP8[getSignalClockAddr(Module)] = 0;
    //    Module.HEAP8[Module._Py_EMSCRIPTEN_SIGNAL_HANDLING] = 1;
    // }
    setCpuLimitNearlyExceededCallback(
      Module.HEAP8,
      getSignalClockAddr(Module),
      Module._Py_EMSCRIPTEN_SIGNAL_HANDLING
    );
    return;
  }
  unreachable(version);
}

const SIGXCPU = 24;

export function clearSignals(Module: Module): void {
  if (Module.API.version === '0.28.2') {
    // In case the previous request was aborted, make sure that:
    // 1. a sigint is waiting in the signal buffer
    // 2. signal handling is off
    //
    // We will turn signal handling on as part of triggering the interrupt, having it on otherwise
    // just wastes cycles.
    Module.Py_EmscriptenSignalBuffer[0] = SIGXCPU;
    Module.HEAPU32[getSignalClockAddr(Module) / 4] = 1;
    Module.HEAPU32[Module._Py_EMSCRIPTEN_SIGNAL_HANDLING / 4] = 0;
  }
}

function compileModuleFromReadOnlyFS(
  Module: Module,
  path: string
): WebAssembly.Module {
  const { node } = Module.FS.lookupPath(path);
  // Get the trusted read function from our private Map, not from the node
  // or filesystem object (which could have been tampered with by user code)
  const trustedRead = getTrustedReadFunc(node);
  if (!trustedRead) {
    throw new PythonUserError(
      'Can only load shared libraries from read only file systems.'
    );
  }
  const stat = node.node_ops.getattr(node);
  const buffer = new Uint8Array(stat.size);
  // Create a minimal stream object and read using trusted read function
  const stream = { node, position: 0 };
  trustedRead(stream, buffer, 0, stat.size, 0);
  return UnsafeEval.newWasmModule(buffer);
}

export function loadPyodide(
  isWorkerd: boolean,
  lockfile: PackageLock,
  indexURL: string,
  customSerializedObjects: CustomSerializedObjects
): Pyodide {
  try {
    const Module = enterJaegerSpan('instantiate_emscripten', () =>
      SetupEmscripten.getModule()
    );
    Module.compileModuleFromReadOnlyFS = compileModuleFromReadOnlyFS;
    Module.API.config.jsglobals = globalThis;
    if (isWorkerd) {
      Module.API.config.indexURL = indexURL;
      Module.API.config.resolveLockFilePromise!(lockfile);
    }
    Module.setUnsafeEval(UnsafeEval);
    Module.setGetRandomValues(getRandomValues);
    Module.setSetTimeout(
      makeSetTimeout(Module),
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
      prepareWasmLinearMemory(Module, customSerializedObjects);
    });

    maybeCollectSnapshot(Module, customSerializedObjects);
    // Mount worker files after doing snapshot upload so we ensure that data from the files is never
    // present in snapshot memory.
    mountWorkerFiles(Module);

    if (Module.API.version === '0.26.0a2') {
      // Finish setting up Pyodide's ffi so we can use the nice Python interface
      // In newer versions we already did this in prepareWasmLinearMemory.
      finalizeBootstrap(Module, customSerializedObjects);
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
    setupRuntimeSignalHandling(Module);
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
