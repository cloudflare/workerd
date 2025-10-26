interface ENV {
  HOME: string;
  [k: string]: string;
}

interface PyodideConfig {
  env: ENV;
  jsglobals: typeof globalThis;
  resolveLockFilePromise?: (lockfile: PackageLock) => void;
  indexURL?: string;
  _makeSnapshot?: boolean;
  lockFileURL: '';
}

type SerializedHiwireValue = { path: string[] } | { serialized: any } | null;

type SnapshotConfig = {
  hiwireKeys: SerializedHiwireValue[];
  immortalKeys: string[];
};

interface API {
  config: PyodideConfig;
  finalizeBootstrap: (
    fromSnapshot?: SnapshotConfig,
    snapshotDeserializer?: (obj: any) => any
  ) => void;
  public_api: Pyodide;
  rawRun: (code: string) => [status: number, err: string];
  initializeStreams: (
    stdin?: any,
    stdout?: (a: string) => void,
    stderr?: (a: string) => void
  ) => void;
  version: '0.26.0a2' | '0.28.2';
  pyodide_base: {
    pyimport_impl: PyCallable;
  };
  serializeHiwireState(serializer: (obj: any) => any): SnapshotConfig;
  pyVersionTuple: [number, number, number];
}

interface LDSO {
  loadedLibsByHandle: {
    [handle: number]: DSO;
  };
  loadedLibsByName: {
    [name: string]: DSO;
  };
}

interface DSO {
  name: string;
  refcount: number;
  global: boolean;
  exports: WebAssembly.Exports;
}

// https://github.com/emscripten-core/emscripten/blob/main/src/lib/libpath.js
interface PATH {
  normalizeArray: (parts: string[], allowAboveRoot: boolean) => string[];
}

type PreRunHook = (mod: Module) => void;

interface EmscriptenSettings {
  preRun: PreRunHook[];
  instantiateWasm: (
    wasmImports: WebAssembly.Imports,
    successCallback: (
      inst: WebAssembly.Instance,
      mod: WebAssembly.Module
    ) => void
  ) => WebAssembly.Exports;
  reportUndefinedSymbolsNoOp: () => void;
  noInitialRun?: boolean;
  API: {
    config: API['config'];
  };
  readyPromise: Promise<Module>;
  rejectReadyPromise: (e: any) => void;
}

interface Module {
  HEAP8: Uint8Array;
  HEAPU32: Uint32Array;
  _dump_traceback: () => void;
  FS: FS;
  API: API;
  ENV: ENV;
  LDSO: LDSO;
  PATH: PATH;
  newDSO: (path: string, opt: object | undefined, handle: string) => DSO;
  _Py_Version: number;
  _py_version_major?: () => number;
  _py_version_minor: () => number;
  _py_version_micro: () => number;
  loadWebAssemblyModule: (
    mod: WebAssembly.Module,
    opt: object,
    path: string,
    localScope: object
  ) => WebAssembly.Exports;
  growMemory(newSize: number): void;
  addRunDependency(x: string): void;
  removeRunDependency(x: string): void;
  noInitialRun: boolean;
  setUnsafeEval(mod: typeof import('internal:unsafe-eval').default): void;
  setGetRandomValues(
    func: typeof import('pyodide-internal:topLevelEntropy/lib').getRandomValues
  ): void;
  setSetTimeout(
    st: typeof setTimeout,
    ct: typeof clearTimeout,
    si: typeof setInterval,
    ci: typeof clearInterval
  ): void;
  getMemory(size: number): number;
  getMemoryPatched(
    Module: Module,
    libName: string,
    handle: number,
    size: number
  ): number;
  promise: Promise<void>;
  reportUndefinedSymbols(): void;
  wasmTable: WebAssembly.Table;
  // Set snapshotDebug to true to print relocation information when loading dynamic libraries. If
  // there are crashes involving snapshots this information can be compared between when creating
  // and using the snapshot and any discrepancy explains the crash.
  snapshotDebug?: boolean;
  getEmptyTableSlot(): number;
  freeTableIndexes: number[];
  LD_LIBRARY_PATH: string;
}
