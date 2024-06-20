
declare namespace UnsafeEval {
  const newWasmModule: (wasm: Uint8Array) => WebAssembly.Module;
}

export default UnsafeEval;
