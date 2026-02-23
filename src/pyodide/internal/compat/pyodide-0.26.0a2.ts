/**
 * Version adapter for Pyodide 0.26.0a2.
 */
import { PythonWorkersInternalError } from 'pyodide-internal:util';
import type { PyodideVersionAdapter } from 'pyodide-internal:compat/compat-api';

function lookupPath(Module: Module, path: string): string | undefined {
  try {
    Module.FS.lookupPath(path);
  } catch (_e) {
    return undefined;
  }
  return path;
}

export const adapter: PyodideVersionAdapter = {
  version: '0.26.0a2',

  // --- Dynamic library loading ---

  resolveLibraryPath(Module: Module, path: string, _rpath: any): string {
    // In 0.26.0a2 we manually search /usr/lib/ and /session/metadata/python_modules/lib/.
    // Newer versions use Module.findLibraryFS() and LD_LIBRARY_PATH instead.
    const result =
      lookupPath(Module, '/usr/lib/' + path) ??
      lookupPath(Module, '/session/metadata/python_modules/lib/' + path);
    if (!result) {
      console.error('Failed to read ', path);
      throw new PythonWorkersInternalError('Should not happen');
    }
    return result;
  },

  reportUndefinedSymbols(_Module: Module): void {
    // No-op in 0.26.0a2.
  },

  // --- Snapshot behavior ---

  snapshotAwareDynlibLoading: false,
  alwaysPreloadDynlibs: true,
  supportsHiwireSerialization: false,
  assertDedicatedSnapshotSupported(): void {
    throw new PythonWorkersInternalError(
      `Dedicated snapshot is not supported for Pyodide version ${this.version}`
    );
  },
  supportsMakeSnapshotConfig: false,

  // --- Bootstrap ordering ---

  earlyFinalizeBootstrap: false,

  // --- Signal handling ---

  setupSignalHandling(
    _Module: Module,
    _setCpuLimitCallback: (
      heap: any,
      clockAddr: number,
      handlingAddr: number
    ) => void
  ): void {
    // No signal handling in 0.26.0a2.
  },

  clearSignals(_Module: Module): void {
    // No signal handling in 0.26.0a2.
  },

  // --- Module import ---

  async pyimportMain(pyodide: Pyodide, moduleName: string): Promise<PyModule> {
    // 0.26.0a2 uses synchronous pyimport.
    return Promise.resolve(pyodide.pyimport(moduleName));
  },

  // --- Patching ---

  getRequiredRuntimePatches(transitiveRequirements: Set<string>): string[] {
    // In 0.26.0a2, httpx and aiohttp patches must be applied at runtime.
    const patches: string[] = [];
    if (transitiveRequirements.has('httpx')) {
      patches.push('httpx');
    }
    if (transitiveRequirements.has('aiohttp')) {
      patches.push('aiohttp');
    }
    return patches;
  },

  async injectLegacyCloudflareNamespace(
    pyodide: Pyodide,
    injectSitePackagesModule: (
      pyodide: Pyodide,
      jsModName: string,
      pyModName: string
    ) => Promise<void>
  ): Promise<void> {
    // In 0.26.0a2, the SDK lived under cloudflare.workers.
    const sitePackages = pyodide.FS.sitePackages;
    pyodide.FS.mkdirTree(`${sitePackages}/cloudflare/workers`);
    await injectSitePackagesModule(
      pyodide,
      'workers-api/src/workers/__init__',
      'cloudflare/workers/__init__'
    );
    await injectSitePackagesModule(
      pyodide,
      'workers-api/src/workers/_workers',
      'cloudflare/workers/_workers'
    );
  },
};
