interface Pyodide {
  _module: Module;
  registerJsModule: (handle: string, mod: object) => void;
}

interface ENV {
  HOME: string;
}

interface API {
  config: {
    env: ENV;
  };
  finalizeBootstrap: () => void;
  public_api: Pyodide;
  rawRun: (code: string) => [status: number, err: string];
}

interface LDSO {
  loadedLibsByHandle: {
    [handle: string]: DSO;
  };
}

interface DSO {
  name: string;
  refcount: number;
  global: boolean;
  exports: WebAssembly.Exports;
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
  ) => WebAssembly.Exports;
}
