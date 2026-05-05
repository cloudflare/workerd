// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

declare module 'pyodide-internal:generated/pyodide.asm.wasm' {
  const pyodideWasmModule: WebAssembly.Module;
  export default pyodideWasmModule;
}

declare module 'pyodide-internal:generated/pyodide.asm' {
  const _createPyodideModule: (es: EmscriptenSettings) => Promise<void>;
}
