// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { rejects } from 'node:assert';
import wasm from 'wasm-js-string';

// Without the wasm_esm_integration_builtins compat flag, the `wasm:*` imports are
// ordinary imports that must be supplied by the embedder.
export const wasmJsStringBuiltinsDisabledTest = {
  async test() {
    await rejects(WebAssembly.instantiate(wasm, {}), {
      name: 'TypeError',
      message: /wasm:js-string/,
    });
  },
};
