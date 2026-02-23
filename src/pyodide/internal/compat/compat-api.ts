/**
 * PyodideVersionAdapter encapsulates all behavior that differs between Pyodide versions.
 *
 * When adding a new Pyodide version:
 * 1. Create a new file pyodide-X.Y.Z.ts implementing this interface
 * 2. Register it in index.ts
 * 3. Existing adapters are frozen — try not to modify them after release unless
 *    there is a critical bugfix or changes in outer code that require it.
 */
export interface PyodideVersionAdapter {
  readonly version: string;

  // === Dynamic library loading (builtin_wrappers.ts) ===

  /**
   * Resolve a non-absolute library path to a filesystem path.
   * 0.26.0a2: searches /usr/lib/ and /session/metadata/python_modules/lib/.
   * Newer versions: delegates to Module.findLibraryFS(), which respects LD_LIBRARY_PATH and RPATH.
   */
  resolveLibraryPath(Module: Module, path: string, rpath: any): string;

  /**
   * Report undefined symbols after loading dynamic libraries.
   * 0.26.0a2: no-op. Newer versions: calls Module.reportUndefinedSymbols().
   * TODO: document why it was needed in 0.26.0a2
   */
  reportUndefinedSymbols(Module: Module): void;

  // === Snapshot behavior (snapshot.ts) ===

  /**
   * Whether dynamic library memory allocation replays addresses from snapshot metadata.
   * If false, uses simple Module.getMemory(). If true, replays from snapshot.
   */
  readonly snapshotAwareDynlibLoading: boolean;

  /**
   * Whether to always preload all .so files regardless of snapshot state.
   * 0.26.0a2: true (always preloads). Newer: false (only when restoring snapshot).
   */
  readonly alwaysPreloadDynlibs: boolean;

  /**
   * Whether the version supports serializing hiwire (JS object) state in snapshots.
   * 0.26.0a2: false. Newer: true.
   */
  readonly supportsHiwireSerialization: boolean;

  /**
   * Assert that dedicated (post-top-level) snapshots are supported.
   * 0.26.0a2: throws PythonWorkersInternalError. Newer: no-op.
   */
  assertDedicatedSnapshotSupported(): void;

  /**
   * Whether to set _makeSnapshot on pyodide config during snapshot creation.
   * 0.26.0a2: false. Newer: true.
   */
  readonly supportsMakeSnapshotConfig: boolean;

  // === Bootstrap ordering (python.ts) ===

  /**
   * Whether finalizeBootstrap runs early (in prepareWasmLinearMemory) or late (in loadPyodide).
   * 0.26.0a2: false (late). Newer: true (early).
   */
  readonly earlyFinalizeBootstrap: boolean;

  // === Signal handling (python.ts) ===

  /** Set up CPU limit signal handling for the runtime. */
  setupSignalHandling(
    Module: Module,
    setCpuLimitCallback: (
      heap: any,
      clockAddr: number,
      handlingAddr: number
    ) => void
  ): void;

  /** Clear pending signals before processing a new request. */
  clearSignals(Module: Module): void;

  // === Module import (python-entrypoint-helper.ts) ===

  /**
   * Import the user's main Python module.
   * 0.26.0a2: synchronous pyimport. Newer: async callPromising.
   */
  pyimportMain(pyodide: Pyodide, moduleName: string): Promise<PyModule>;

  // === Version-specific patches (python-entrypoint-helper.ts) ===

  /**
   * Return list of package names that require runtime patches for this version.
   */
  getRequiredRuntimePatches(transitiveRequirements: Set<string>): string[];

  /**
   * Inject the legacy cloudflare.workers namespace into site-packages.
   * 0.26.0a2: creates cloudflare/workers directory and injects __init__ and _workers modules.
   * Newer: no-op (only the top-level 'workers' package is used).
   */
  injectLegacyCloudflareNamespace(
    pyodide: Pyodide,
    injectSitePackagesModule: (
      pyodide: Pyodide,
      jsModName: string,
      pyModName: string
    ) => Promise<void>
  ): Promise<void>;
}
