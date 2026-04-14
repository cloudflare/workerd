// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

declare module 'pyodide-internal:introspection.py' {
  const value: Uint8Array;
  export default value;
}

declare module 'pyodideRuntime-internal:emscriptenSetup' {
  function instantiateEmscriptenModule(
    isWorkerd: boolean,
    pythonStdlib: ArrayBuffer,
    pyodideWasmModule: WebAssembly.Module
  ): Promise<Module>;
  export { instantiateEmscriptenModule };
}

declare module 'pyodideRuntime-internal:python_stdlib.zip' {
  const value: ArrayBuffer;
  export default value;
}

declare module 'pyodideRuntime-internal:pyodide.asm.wasm' {
  const value: WebAssembly.Module;
  export default value;
}
