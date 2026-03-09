// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

declare namespace UnsafeEval {
  const newWasmModule: (wasm: Uint8Array) => WebAssembly.Module;
}

export default UnsafeEval;
