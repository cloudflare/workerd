declare type Handler = (...args: any[]) => any;

interface PyModule {
  [handlerName: string]: {
    callRelaxed: (...args: any[]) => any;
  };
}

interface Pyodide {
  _module: Module;
  runPython: (code: string) => void;
  registerJsModule: (handle: string, mod: object) => void;
  pyimport: (moduleName: string) => PyModule;
  FS: FS;
  loadPackage: (names: string | string[], options: object) => Promise<any[]>;
  setStdout: (options?: any) => void;
  setStderr: (options?: any) => void;
}
