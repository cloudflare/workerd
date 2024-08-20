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
  noInitialRun: boolean;
  API: {
    config: API['config'];
  };
}
