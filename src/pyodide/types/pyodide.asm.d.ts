declare module "pyodide-internal:generated/pyodide.asm.wasm" {
  const pyodideWasmModule: WebAssembly.Module;
  export default pyodideWasmModule;
}

declare module "pyodide-internal:generated/pyodide.asm" {
  const _createPyodideModule: (es: EmscriptenSettings) => Module;
}
