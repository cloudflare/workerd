// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual } from 'node:assert';

export const test = {
  test() {
    // Without the wasm_memory_discard compat flag the JS API is not installed.
    strictEqual(WebAssembly.Memory.prototype.discard, undefined);
  },
};
