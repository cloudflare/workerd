// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { strictEqual } from 'node:assert';

export const TextDecoderUtf16OddOffset = {
  async test() {
    const decoder = new TextDecoder('utf-16le');
    const buffer = new ArrayBuffer(200);
    const view = new Uint8Array(buffer, 3, 20);
    for (let i = 0; i < 20; i += 2) {
      view[i] = 0x42;
      view[i + 1] = 0x00;
    }
    const result = decoder.decode(view);
    strictEqual(result, 'BBBBBBBBBB');
  },
};
