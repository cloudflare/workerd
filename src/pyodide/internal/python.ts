// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

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
  PROCESS_PTH_FILES,
  SHOULD_ABORT_ISOLATE_ON_FATAL_ERROR,
  setCpuLimitNearlyExceededCallback,
} from 'pyodide-internal:metadata';
import { default as FatalReporter } from 'pyodide-internal:fatal-reporter';
import { default as cloudflareWorkers } from 'cloudflare-internal:workers';

import { default as UnsafeEval } from 'internal:unsafe-eval';
import {
  PythonUserError,
  PythonWorkersInternalError,
  loadPythonMod,
  reportError,
  setInternalErrorReporter,
  unreachable,
} from 'pyodide-internal:util';
import { loadPackages } from 'pyodide-internal:loadPackage';
import { default as MetadataReader } from 'pyodide-internal:runtime-generated/metadata';
import { default as setupPythonSearchPathSource } from 'pyodide-internal:setup_python_search_path.py';
import { TRANSITIVE_REQUIREMENTS, IS_WORKERD } from 'pyodide-internal:metadata';
import { getTrustedReadFunc } from 'pyodide-internal:readOnlyFS';
import { PyodideVersion } from 'pyodide-internal:const';
import { default as pythonStdlibZip } from 'pyodideRuntime-internal:python_stdlib.zip';
import { default as pyodideAsmWasm } from 'pyodideRuntime-internal:pyodide.asm.wasm';
import { instantiateEmscriptenModule } from 'pyodideRuntime-internal:emscriptenSetup';
import { createImportProxy } from 'pyodide-internal:serializeJsModule';

// Wire the PythonWorkersInternalError constructor's reporter to the C++ FatalReporter module.
// See util.ts for why this indirection is needed (pool bundling constraints).
// TODO: Remove once the Python pool is gone and util.ts can import FatalReporter directly.
setInternalErrorReporter(() => {
  FatalReporter.reportPythonWorkersInternalError();
});

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
  if (Module.API.version !== PyodideVersion.V0_26_0a2) {
    finalizeBootstrap(Module, customSerializedObjects);
  }
}

type SetupPythonSearchPathMod = {
  __dict__: PyDict;
  setup_python_search_path: PyCallable;
  destroy(): void;
};

function setupPythonSearchPath(pyodide: Pyodide): void {
  const mod = loadPythonMod(
    pyodide,
    'setup_python_search_path',
    setupPythonSearchPathSource
  ) as SetupPythonSearchPathMod;
  mod.setup_python_search_path.callKwargs({
    LEGACY_VENDOR_PATH,
    PROCESS_PTH_FILES,
  });
  mod.destroy();
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
  if (Module.API.version !== PyodideVersion.V0_28_2) {
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
  if (version === PyodideVersion.V0_26_0a2) {
    return;
  }
  if (version === PyodideVersion.V0_28_2) {
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
  if (Module.API.version === PyodideVersion.V0_28_2) {
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

export async function loadPyodide(
  isWorkerd: boolean,
  lockfile: PackageLock,
  customSerializedObjects: CustomSerializedObjects
): Promise<Pyodide> {
  try {
    const Module = await enterJaegerSpan('instantiate_emscripten', () =>
      instantiateEmscriptenModule(IS_WORKERD, pythonStdlibZip, pyodideAsmWasm)
    );
    Module.compileModuleFromReadOnlyFS = compileModuleFromReadOnlyFS;
    if (Module.API.version === PyodideVersion.V0_28_2) {
      Module.API.config.jsglobals = createImportProxy(
        'global this',
        globalThis
      );
    } else {
      Module.API.config.jsglobals = globalThis;
    }
    if (isWorkerd) {
      Module.API.config.resolveLockFilePromise!(lockfile);
    }
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

    if (Module.API.version === PyodideVersion.V0_26_0a2) {
      // Finish setting up Pyodide's ffi so we can use the nice Python interface
      // In newer versions we already did this in prepareWasmLinearMemory.
      finalizeBootstrap(Module, customSerializedObjects);
    }
    const pyodide = Module.API.public_api;

    validatePyodideVersion(pyodide);
    setupPythonSearchPath(pyodide);
    setupRuntimeSignalHandling(Module);
    Module.API.on_fatal = (error: unknown): void => {
      try {
        FatalReporter.reportFatal(String(error));
      } catch (_e) {
        FatalReporter.reportFatal('Internal error reporting fatal error');
      }
      if (SHOULD_ABORT_ISOLATE_ON_FATAL_ERROR) {
        cloudflareWorkers.abortIsolate(
          `Python worker fatal error: ${String(error)}`
        );
      }
    };
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
