// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { Buffer } from 'node:buffer';
import { strictEqual } from 'node:assert';

export const BufferConcatResizeDuringGetter = {
  async test() {
    const ab = new ArrayBuffer(1024, { maxByteLength: 2048 });
    const view = new Uint8Array(ab);
    view.fill(0x41);

    const arr = [view, new Uint8Array(0)];

    Object.defineProperty(arr, '1', {
      get() {
        ab.resize(0);
        return new Uint8Array(0);
      },
      enumerable: true,
      configurable: true,
    });

    const result = Buffer.concat(arr, 1024);
    strictEqual(result.length, 1024);
    strictEqual(
      result.every((b) => b === 0),
      true
    );
  },
};
