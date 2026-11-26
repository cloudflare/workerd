// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { deepStrictEqual } from 'node:assert';

// Without pedantic_wpt, SubtleCrypto still accepts a view onto a
// SharedArrayBuffer.
export const sharedViewAccepted = {
  async test() {
    const memory = new WebAssembly.Memory({
      shared: true,
      initial: 1,
      maximum: 1,
    });
    const view = new Uint8Array(memory.buffer, 0, 4);
    view.set([1, 2, 3, 4]);
    const fromShared = await crypto.subtle.digest('SHA-256', view);
    const fromCopy = await crypto.subtle.digest(
      'SHA-256',
      new Uint8Array([1, 2, 3, 4])
    );
    deepStrictEqual(new Uint8Array(fromShared), new Uint8Array(fromCopy));
  },
};
