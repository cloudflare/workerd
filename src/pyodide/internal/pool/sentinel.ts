// This is https://github.com/pyodide/pyodide/blob/main/src/core/sentinel.ts.
// It goes into `pyodide.js` not `pyodide.asm.js`. We don't use `pyodide.js` so we have to reproduce
// it here.

// TODO(soon): Remove this once TypeScript recognizes base64/hex methods
// Type declarations for V8 14.1+ ArrayBuffer base64/hex methods
// https://tc39.es/proposal-arraybuffer-base64/
declare global {
  interface Uint8ArrayConstructor {
    fromBase64(
      base64String: string,
      options?: {
        alphabet?: 'base64' | 'base64url';
        lastChunkHandling?: 'loose' | 'strict' | 'stop-before-partial';
      }
    ): Uint8Array;
  }
}

// This string is https://github.com/pyodide/pyodide/blob/main/src/core/sentinel.wat assembled and
// base64 encoded.
const sentinelWasm = Uint8Array.fromBase64(
  'AGFzbQEAAAABDANfAGAAAW9gAW8BfwMDAgECByECD2NyZWF0ZV9zZW50aW5lbAAAC2lzX3NlbnRpbmVsAAEKEwIHAPsBAPsbCwkAIAD7GvsUAAs'
);

export async function getSentinelImport() {
  const module: WebAssembly.Module = new WebAssembly.Module(sentinelWasm);
  const instance = await WebAssembly.instantiate(module);
  return instance.exports;
}
