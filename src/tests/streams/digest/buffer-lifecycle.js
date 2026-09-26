// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Input buffer lifecycle: the hash update runs inside write(), so the
// chunk's bytes are consumed before the write settles — mutation after the
// write cannot change the digest, and the chunk is never retained.

import { deepStrictEqual, strictEqual } from 'node:assert';
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

// Views already detached, or left out of bounds by a shrink, at write()
// time hash as empty — DataViews too, whose byteLength getter throws where
// a typed array's reports 0 — and the stream keeps hashing (parity).
export const degenerateViewsHashAsEmpty = {
  async test() {
    const stream = new crypto.DigestStream('crc32');
    const writer = stream.getWriter();
    for (const View of [Uint8Array, DataView]) {
      const ab = new ArrayBuffer(8);
      const detached = new View(ab, 2, 4);
      ab.transfer();
      await writer.write(detached);
      const rab = new ArrayBuffer(8, { maxByteLength: 8 });
      const outOfBounds = new View(rab, 4, 4);
      rab.resize(2);
      await writer.write(outOfBounds);
    }
    await writer.write(new Uint8Array([1, 2, 3, 4]));
    await writer.close();
    deepStrictEqual(
      new Uint8Array(await stream.digest),
      await digestOf('crc32', new Uint8Array([1, 2, 3, 4]))
    );
    strictEqual(stream.bytesWritten, 4n);
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
