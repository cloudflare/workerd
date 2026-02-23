/**
 * Version adapter for Pyodide 0.28.2.
 */
import type { PyodideVersionAdapter } from 'pyodide-internal:compat/compat-api';

const SIGXCPU = 24;

/**
 * Returns the address of the emscripten signal clock in linear memory.
 *
 * This is the address of _Py_emscripten_signal_clock:
 * https://github.com/python/cpython/blob/main/Python/emscripten_signal.c#L42
 *
 * Since the symbol isn't exported, we can't access it directly. Instead, we used wasm-objdump and
 * searched for the call site to _Py_CheckEmscriptenSignals_Helper(), then read the offset out of
 * the assembly code.
 *
 * TODO: Export this symbol in the next Pyodide release so we can stop using the magic number.
 */
function getSignalClockAddr(Module: Module): number {
  const emscripten_signal_clock_offset = 3171536;
  return Module.___memory_base.value + emscripten_signal_clock_offset;
}

export const adapter: PyodideVersionAdapter = {
  version: '0.28.2',

  // --- Dynamic library loading ---

  resolveLibraryPath(Module: Module, path: string, rpath: any): string {
    return Module.findLibraryFS(path, rpath);
  },

  reportUndefinedSymbols(Module: Module): void {
    Module.reportUndefinedSymbols();
  },

  // --- Snapshot behavior ---

  snapshotAwareDynlibLoading: true,
  alwaysPreloadDynlibs: false,
  supportsHiwireSerialization: true,
  assertDedicatedSnapshotSupported(): void {
    // Dedicated snapshots are supported in 0.28.2 — no-op.
  },
  supportsMakeSnapshotConfig: true,

  // --- Bootstrap ordering ---

  earlyFinalizeBootstrap: true,

  // --- Signal handling ---

  setupSignalHandling(
    Module: Module,
    setCpuLimitCallback: (
      heap: any,
      clockAddr: number,
      handlingAddr: number
    ) => void
  ): void {
    // The callback sets signal_clock to 0 and signal_handling to 1. It has to be in C++ because we
    // don't hold the isolate lock when we call it.
    setCpuLimitCallback(
      Module.HEAP8,
      getSignalClockAddr(Module),
      Module._Py_EMSCRIPTEN_SIGNAL_HANDLING
    );
  },

  clearSignals(Module: Module): void {
    // In case the previous request was aborted, make sure that:
    // 1. a sigint is waiting in the signal buffer
    // 2. signal handling is off
    //
    // We will turn signal handling on as part of triggering the interrupt, having it on otherwise
    // just wastes cycles.
    Module.Py_EmscriptenSignalBuffer[0] = SIGXCPU;
    Module.HEAPU32[getSignalClockAddr(Module) / 4] = 1;
    Module.HEAPU32[Module._Py_EMSCRIPTEN_SIGNAL_HANDLING / 4] = 0;
  },

  // --- Module import ---

  async pyimportMain(pyodide: Pyodide, moduleName: string): Promise<PyModule> {
    // Newer versions use async callPromising for JSPI support.
    return await pyodide._module.API.pyodide_base.pyimport_impl.callPromising(
      moduleName
    ) as PyModule;
  },

  // --- Patching ---

  getRequiredRuntimePatches(transitiveRequirements: Set<string>): string[] {
    // In 0.28.2, aiohttp patches must be applied at runtime.
    const patches: string[] = [];
    if (transitiveRequirements.has('aiohttp')) {
      patches.push('aiohttp');
    }
    return patches;
  },

  async injectLegacyCloudflareNamespace(
    _pyodide: Pyodide,
    _injectSitePackagesModule: (
      pyodide: Pyodide,
      jsModName: string,
      pyModName: string
    ) => Promise<void>
  ): Promise<void> {
    // No-op in 0.28.2: the legacy cloudflare.workers namespace is no longer needed.
  },
};
