declare type Handler = (...args: any[]) => any;

interface PyModule {
  [handlerName: string]: {
    callRelaxed: (...args: any[]) => any;
  };
}

interface Pyodide {
  _module: Module;
  registerJsModule: (handle: string, mod: object) => void;
  pyimport: (moduleName: string) => PyModule;
  FS: FS;
  site_packages: string;
  loadPackage: (names: string | string[], options: object) => Promise<any[]>;
}
