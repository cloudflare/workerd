declare type AnyClass<T = any> = new (...args: any[]) => T;

declare type Handler = (...args: any[]) => any;

declare type PyCallable = ((...args: any[]) => any) & {
  call: (...args: any[]) => any;
  callRelaxed: (...args: any[]) => any;
  callPromising: (...args: any[]) => any;
  callWithOptions?: (
    options: { relaxed?: boolean; promising?: boolean; kwargs?: boolean },
    ...args: any[]
  ) => any;
};

declare type PyDict = object;

interface PyModule {
  [handlerName: string]: PyCallable;
}

interface Pyodide {
  _module: Module;
  runPython: (
    code: string,
    opts?: { globals?: PyDict; filename?: string }
  ) => any;
  registerJsModule: (handle: string, mod: object) => void;
  pyimport: (moduleName: string) => PyModule;
  FS: FS;
  loadPackage: (names: string | string[], options: object) => Promise<any[]>;
  setStdout: (options?: any) => void;
  setStderr: (options?: any) => void;
  version: API['version'];
}
