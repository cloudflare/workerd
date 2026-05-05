// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

declare type AnyClass<T = any> = new (...args: any[]) => T;

declare type Handler = (...args: any[]) => any;

declare type PyCallable = ((...args: any[]) => any) & {
  call: (...args: any[]) => any;
  callRelaxed: (...args: any[]) => any;
  callPromising: (...args: any[]) => any;
  callKwargs: (...args: any[]) => any;
  callWithOptions?: (
    options: { relaxed?: boolean; promising?: boolean; kwargs?: boolean },
    ...args: any[]
  ) => any;
  destroy(): void;
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
