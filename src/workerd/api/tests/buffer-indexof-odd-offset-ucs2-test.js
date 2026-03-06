// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { Buffer } from 'node:buffer';
import { strictEqual } from 'node:assert';

export const BufferIndexOfUnalignedUtf16 = {
  async test() {
    const ab = new ArrayBuffer(32);
    const view = new Uint8Array(ab, 1, 30);

    for (let i = 0; i < view.length; i++) {
      view[i] = i % 2 === 0 ? 0x41 : 0x42;
    }

    const buf = Buffer.from(view.buffer, view.byteOffset, view.byteLength);
    const idx = buf.indexOf('AB', 0, 'ucs2');
    strictEqual(typeof idx, 'number');
  },
};
