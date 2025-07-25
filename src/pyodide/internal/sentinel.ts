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
    const chunk1 = base64Lookup[input.charCodeAt(inIdx++)];
    const chunk2 = base64Lookup[input.charCodeAt(inIdx++)];
    const chunk3 = base64Lookup[input.charCodeAt(inIdx++)];
    const chunk4 = base64Lookup[input.charCodeAt(inIdx++)];
    output[outIdx++] = (chunk1 << 2) | (chunk2 >> 4);
    output[outIdx++] = (chunk2 << 4) | (chunk3 >> 2);
    output[outIdx++] = (chunk3 << 6) | chunk4;
  }
  return output;
}

const sentinelWasm = decodeBase64(
  'AGFzbQEAAAABDANfAGAAAW9gAW8BfwMDAgECByECD2NyZWF0ZV9zZW50aW5lbAAAC2lzX3NlbnRpbmVsAAEKEwIHAPsBAPsbCwkAIAD7GvsUAAs'
);

const sentinelInstancePromise: Promise<WebAssembly.Instance> =
  (async function () {
    const module = await WebAssembly.compile(sentinelWasm);
    return await WebAssembly.instantiate(module);
  })();

export async function getSentinelImport() {
  let e = await sentinelInstancePromise;
  return e.exports;
}
