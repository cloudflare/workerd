// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The patched Emscripten glue, the only JavaScript module in the Pyodide bundle.
declare module 'pyodideRuntime-internal:pyodide.asm' {
  const _createPyodideModule: (es: EmscriptenSettings) => Promise<void>;
  export { _createPyodideModule };
}
