interface EmscriptenSettings {
  preRun: ((mod: Module) => void)[];
  instantiateWasm: (
    wasmImports: WebAssembly.Imports,
    successCallback: (
      inst: WebAssembly.Instance,
      mod: WebAssembly.Module
    ) => void
  ) => WebAssembly.Exports;
  reportUndefinedSymbolsNoOp: () => void;
  API: {
    config: API['config'];
  };
  readyPromise: Promise<Module>;
}
