interface ENV {
  HOME: string;
  [k: string]: string;
}

interface PyodideConfig {
  env: ENV;
  jsglobals: any;
  resolveLockFilePromise?: (lockfile: PackageLock) => void;
  indexURL?: string;
  _makeSnapshot?: boolean;
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
  version: '0.26.0a2' | '0.27.5';
  pyodide_base: {
    pyimport_impl: PyCallable;
  };
  serializeHiwireState(Module: Module): SnapshotConfig;
}

interface LDSO {
  loadedLibsByHandle: {
    [handle: string]: DSO;
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
}

interface Module {
  HEAP8: Uint8Array;
  _dump_traceback: () => void;
  FS: FS;
  API: API;
  ENV: ENV;
  LDSO: LDSO;
  newDSO: (path: string, opt: object | undefined, handle: string) => DSO;
  _py_version_major: () => number;
  _py_version_minor: () => number;
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
  getMemory(size: number): number;
  getMemoryPatched(
    Module: Module,
    libName: string,
    handle: number,
    size: number
  ): number;
  promise: Promise<void>;
}
