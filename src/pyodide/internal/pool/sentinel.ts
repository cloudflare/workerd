// This is https://github.com/pyodide/pyodide/blob/main/src/core/sentinel.ts.
// It goes into `pyodide.js` not `pyodide.asm.js`. We don't use `pyodide.js` so we have to reproduce
// it here.

// Per https://github.com/tc39/proposal-arraybuffer-base64/issues/51#issuecomment-3010378969, we
// should be able to replace `decodeBase64()` with `Uint8Array.fromBase64()` once we update to a v8
// released after September 2nd.
const base64Lookup = new Uint8Array(128);
for (let t = 0; t < 64; t++) {
  let idx;
  if (t < 26) {
    idx = t + 65;
  } else if (t < 52) {
    idx = t + 71;
  } else if (t < 62) {
    idx = t - 4;
  } else {
    idx = t * 4 - 205;
  }
  base64Lookup[idx] = t;
}

function decodeBase64(input: string): Uint8Array {
  const outputSize = ((input.length * 3) / 4) | 0;
  const output = new Uint8Array(outputSize);
  let inIdx = 0;
  let outIdx = 0;
  while (inIdx < input.length) {
    const chunk1 = base64Lookup[input.charCodeAt(inIdx++)]!;
    const chunk2 = base64Lookup[input.charCodeAt(inIdx++)]!;
    const chunk3 = base64Lookup[input.charCodeAt(inIdx++)]!;
    const chunk4 = base64Lookup[input.charCodeAt(inIdx++)]!;
    output[outIdx++] = (chunk1 << 2) | (chunk2 >> 4);
    output[outIdx++] = (chunk2 << 4) | (chunk3 >> 2);
    output[outIdx++] = (chunk3 << 6) | chunk4;
  }
  return output;
}

// This string is https://github.com/pyodide/pyodide/blob/main/src/core/sentinel.wat assembled and
// hex encoded.
const sentinelWasm = decodeBase64(
  'AGFzbQEAAAABDANfAGAAAW9gAW8BfwMDAgECByECD2NyZWF0ZV9zZW50aW5lbAAAC2lzX3NlbnRpbmVsAAEKEwIHAPsBAPsbCwkAIAD7GvsUAAs'
);

export async function getSentinelImport() {
  const module: WebAssembly.Module = new WebAssembly.Module(sentinelWasm);
  const instance = await WebAssembly.instantiate(module);
  return instance.exports;
}
