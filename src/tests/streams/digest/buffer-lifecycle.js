// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Input buffer lifecycle: the hash update runs inside write(), so the
// chunk's bytes are consumed before the write settles — mutation after the
// write cannot change the digest, and the chunk is never retained.

import { deepStrictEqual } from 'node:assert';
import { digestOf } from 'digest-vectors';

export const mutationAfterWriteIsInvisible = {
  async test() {
    const buf = new Uint8Array([1, 2, 3, 4]);
    const stream = new crypto.DigestStream('crc32');
    const writer = stream.getWriter();
    await writer.write(buf);
    buf.fill(0xff);
    await writer.close();
    deepStrictEqual(
      new Uint8Array(await stream.digest),
      await digestOf('crc32', new Uint8Array([1, 2, 3, 4]))
    );
  },
};

export const detachAfterWriteIsInvisible = {
  async test() {
    const buf = new Uint8Array([1, 2, 3, 4]);
    const stream = new crypto.DigestStream('crc32');
    const writer = stream.getWriter();
    await writer.write(buf);
    buf.buffer.transfer();
    await writer.close();
    deepStrictEqual(
      new Uint8Array(await stream.digest),
      await digestOf('crc32', new Uint8Array([1, 2, 3, 4]))
    );
  },
};

export const lyingMetadataNeverConsulted = {
  async test() {
    // Buffer metadata comes from internal slots; shadowing own getters are
    // never invoked.
    const view = new TextEncoder().encode('real');
    for (const key of ['byteLength', 'byteOffset', 'buffer']) {
      Object.defineProperty(view, key, {
        get() {
          throw new Error(`${key} getter must not be called`);
        },
      });
    }
    const stream = new crypto.DigestStream('md5');
    const writer = stream.getWriter();
    await writer.write(view);
    await writer.close();
    deepStrictEqual(
      new Uint8Array(await stream.digest),
      await digestOf('md5', 'real')
    );
  },
};
