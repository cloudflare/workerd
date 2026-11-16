// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { deepStrictEqual, strictEqual } from 'node:assert';

export const text = {
  async test() {
    // Invalid sequences are replaced, and a byte order mark is kept.
    const blob = new Blob([
      new Uint8Array([0xef, 0xbb, 0xbf, 0x61, 0xff, 0x62]),
    ]);
    strictEqual(await blob.text(), '\ufeffa\ufffdb');
    strictEqual(await new Blob().text(), '');
  },
};

export const copies = {
  async test() {
    const blob = new Blob(['ab']);
    const bytes = await blob.bytes();
    strictEqual(bytes instanceof Uint8Array, true);
    deepStrictEqual([...bytes], [97, 98]);
    bytes[0] = 0;
    strictEqual(await blob.text(), 'ab');
    const buffer = await blob.arrayBuffer();
    strictEqual(buffer instanceof ArrayBuffer, true);
    strictEqual(buffer.byteLength, 2);
    // A slice's copy holds only the slice.
    strictEqual((await blob.slice(1).arrayBuffer()).byteLength, 1);
    deepStrictEqual([...(await blob.slice(1).bytes())], [98]);
  },
};

export const stream = {
  async test() {
    const stream = new Blob(['ab', 'cd']).slice(1).stream();
    strictEqual(stream instanceof ReadableStream, true);
    strictEqual(await new Response(stream).text(), 'bcd');
    // Each call is a new stream.
    const blob = new Blob(['x']);
    strictEqual(blob.stream() === blob.stream(), false);
  },
};

export const pollution = {
  async test() {
    const saved = {
      slice: Uint8Array.prototype.slice,
      subarray: Uint8Array.prototype.subarray,
      set: Uint8Array.prototype.set,
      ctor: Object.getOwnPropertyDescriptor(
        Uint8Array.prototype,
        'constructor'
      ),
    };
    const poison = () => {
      throw new Error('polluted');
    };
    try {
      Uint8Array.prototype.slice = poison;
      Uint8Array.prototype.subarray = poison;
      Uint8Array.prototype.set = poison;
      Object.defineProperty(Uint8Array.prototype, 'constructor', {
        get: poison,
        configurable: true,
      });
      for (const index of ['0', '1']) {
        Object.defineProperty(Array.prototype, index, {
          set: poison,
          configurable: true,
        });
      }
      const blob = new Blob(['abc', new Uint8Array([100])]).slice(1);
      strictEqual(await blob.text(), 'bcd');
      deepStrictEqual([...(await blob.bytes())], [98, 99, 100]);
      strictEqual((await blob.arrayBuffer()).byteLength, 3);
    } finally {
      Uint8Array.prototype.slice = saved.slice;
      Uint8Array.prototype.subarray = saved.subarray;
      Uint8Array.prototype.set = saved.set;
      Object.defineProperty(Uint8Array.prototype, 'constructor', saved.ctor);
      delete Array.prototype[0];
      delete Array.prototype[1];
    }
  },
};
